// SPDX-License-Identifier: LGPL-2.1-or-later
// DXBC SHDR/SHEX token stream bounds. Rust walks exactly the tokens the C++
// CShaderCodeParser reads for one instruction and checks them against both the
// instruction length and the program extent before C++ decodes that instruction.
use super::cache_wire::{input, u32_at};

const OPCODE_COUNT: u32 = 236;
const MAX_OPERANDS: u8 = 8;
// fxc emits relative indices on temps/inputs with immediate indices only. The
// limit bounds recursion here and in the C++ operand decoder.
const MAX_OPERAND_DEPTH: u32 = 4;

const CUSTOMDATA: u32 = 53;
const IMMEDIATE32: u32 = 4;
const IMMEDIATE64: u32 = 5;

enum Error {
    InvalidArgument = 1,
    ProgramHeader = 2,
    ProgramLength = 3,
    InstructionLength = 4,
    Opcode = 5,
    Truncated = 6,
    Operand = 7,
}
type Result<T, E = Error> = core::result::Result<T, E>;

#[repr(C)]
pub struct Program {
    version_token: u32,
    length_tokens: u32,
}

#[repr(C)]
pub struct Instruction {
    opcode: u32,
    length_tokens: u32,
}

struct Cursor<'a> {
    bytes: &'a [u8],
    position: u32,
    end: u32,
}

impl Cursor<'_> {
    fn token(&mut self) -> Result<u32> {
        if self.position >= self.end {
            return Err(Error::Truncated);
        }
        let value = u32_at(self.bytes, self.position as u64 * 4).ok_or(Error::Truncated)?;
        self.position += 1;
        Ok(value)
    }

    fn peek(&self) -> Result<u32> {
        if self.position >= self.end {
            return Err(Error::Truncated);
        }
        u32_at(self.bytes, self.position as u64 * 4).ok_or(Error::Truncated)
    }

    fn skip(&mut self, count: u32) -> Result<()> {
        let position = self.position.checked_add(count).ok_or(Error::Truncated)?;
        if position > self.end {
            return Err(Error::Truncated);
        }
        self.position = position;
        Ok(())
    }

    // Mirrors CShaderCodeParser::ParseOperand and ParseIndex.
    fn operand(&mut self, depth: u32) -> Result<()> {
        if depth >= MAX_OPERAND_DEPTH {
            return Err(Error::Operand);
        }
        let token = self.token()?;
        let kind = (token >> 12) & 0xff;
        let components = match token & 3 {
            1 => 1,
            2 => 4,
            _ => 0,
        };
        let immediate = kind == IMMEDIATE32 || kind == IMMEDIATE64;
        let mut dimensions = 0;
        if !immediate {
            if token & 3 == 2 && (token >> 2) & 3 == 3 {
                return Err(Error::Operand);
            }
            dimensions = (token >> 20) & 3;
        }
        if token >> 31 != 0 {
            self.token()?;
        }
        if immediate {
            self.skip(components)?;
        }
        for dimension in 0..dimensions {
            match (token >> (22 + 3 * dimension)) & 3 {
                0 => self.skip(1)?,
                1 => self.skip(2)?,
                2 => self.operand(depth + 1)?,
                _ => {
                    self.skip(1)?;
                    self.operand(depth + 1)?;
                }
            }
        }
        Ok(())
    }

    fn operands(&mut self, count: u32) -> Result<()> {
        for _ in 0..count {
            self.operand(0)?;
        }
        Ok(())
    }
}

fn program(bytes: &[u8]) -> Result<Program> {
    let version_token = u32_at(bytes, 0).ok_or(Error::ProgramHeader)?;
    let length_tokens = u32_at(bytes, 4).ok_or(Error::ProgramHeader)?;
    if length_tokens < 2 || length_tokens as u64 * 4 > bytes.len() as u64 {
        return Err(Error::ProgramLength);
    }
    Ok(Program { version_token, length_tokens })
}

// Mirrors the token reads of CShaderCodeParser::ParseInstruction.
fn instruction(bytes: &[u8], counts: &[u8], offset: u32) -> Result<Instruction> {
    let program = program(bytes)?;
    if offset < 2 || offset >= program.length_tokens {
        return Err(Error::InvalidArgument);
    }
    let major = (program.version_token >> 4) & 0xf;
    let minor = program.version_token & 0xf;
    let sm51 = (major > 5 || major == 5 && minor > 0) as u32;
    let mut cursor = Cursor { bytes, position: offset, end: program.length_tokens };
    let token = cursor.token()?;
    let opcode = token & 0x7ff;
    if opcode >= OPCODE_COUNT {
        return Err(Error::Opcode);
    }
    let operands = counts[opcode as usize];
    if operands > MAX_OPERANDS {
        return Err(Error::InvalidArgument);
    }
    let mut length = (token >> 24) & 0x7f;
    let extended = token >> 31 != 0;
    if opcode == CUSTOMDATA && extended {
        // Bits 11..31 are the custom-data class here; no class sets bit 31,
        // and the C++ decoder would treat it as an extended opcode token.
        return Err(Error::Opcode);
    }
    if extended && (opcode == 145 || opcode == 146) {
        // Interface and function table declarations may exceed seven bits.
        length = cursor.token()?;
    } else if extended {
        for _ in 0..3 {
            if cursor.token()? >> 31 == 0 {
                break;
            }
        }
    }
    match opcode {
        CUSTOMDATA => {
            let declared = cursor.peek()?;
            length = declared.max(2);
            cursor.skip(length.checked_sub(cursor.position - offset).ok_or(Error::InstructionLength)?)?;
        }
        144 | 104 | 94 | 206 | 152 | 153 | 154 => cursor.skip(1)?,
        // The decoder asserts these exact lengths, so a mismatch must not reach it.
        145 => {
            cursor.skip(1)?;
            let table = cursor.token()?;
            cursor.skip(table)?;
            if length as u64 != 3 + extended as u64 + table as u64 {
                return Err(Error::InstructionLength);
            }
        }
        146 => {
            cursor.skip(2)?;
            let table = cursor.token()? & 0xffff;
            cursor.skip(table)?;
            if length as u64 != 4 + extended as u64 + table as u64 {
                return Err(Error::InstructionLength);
            }
        }
        120 => {
            cursor.skip(1)?;
            cursor.operand(0)?;
        }
        88 | 156 | 158 => {
            cursor.operand(0)?;
            cursor.skip(1 + sm51)?;
        }
        90 | 157 | 161 => {
            cursor.operand(0)?;
            cursor.skip(sm51)?;
        }
        162 => {
            cursor.operand(0)?;
            cursor.skip(1 + sm51)?;
        }
        143 | 95 | 101 | 98 => cursor.operand(0)?,
        97 | 96 | 100 | 99 | 103 | 102 | 91 | 159 => {
            cursor.operand(0)?;
            cursor.skip(1)?;
        }
        89 => {
            cursor.operand(0)?;
            cursor.skip(2 * sm51)?;
        }
        160 => {
            cursor.operand(0)?;
            cursor.skip(2)?;
        }
        105 | 155 => cursor.skip(3)?,
        92 | 93 | 106 | 147 | 148 | 149 | 150 | 151 | 190 => {}
        61 => cursor.operands(3)?,
        111 | 5 => cursor.operands(2)?,
        31 | 3 | 8 | 63 | 13 => cursor.operands(1)?,
        _ => cursor.operands(operands as u32)?,
    }
    // The next instruction starts at offset + length: it must advance, cover
    // every token decoded above, and stay inside the program.
    let end = offset as u64 + length as u64;
    if length == 0 || end < cursor.position as u64 || end > program.length_tokens as u64 {
        return Err(Error::InstructionLength);
    }
    Ok(Instruction { opcode, length_tokens: length })
}

unsafe fn publish<T>(out: *mut T, result: Result<T>) -> u32 {
    if out.is_null() {
        return Error::InvalidArgument as u32;
    }
    match result {
        Ok(value) => {
            unsafe { out.write(value) };
            0
        }
        Err(error) => error as u32,
    }
}

#[no_mangle]
pub unsafe extern "C" fn dxmt_dxbc_program_parse(data: *const u8, length: u64, out: *mut Program) -> u32 {
    let result = (|| program(unsafe { input(data, length) }.ok_or(Error::InvalidArgument)?))();
    unsafe { publish(out, result) }
}

/// `operand_counts` holds one generic operand count per opcode below 236.
#[no_mangle]
pub unsafe extern "C" fn dxmt_dxbc_instruction_parse(
    data: *const u8, length: u64, operand_counts: *const u8, opcode_count: u32, token_offset: u32,
    out: *mut Instruction,
) -> u32 {
    let result = (|| {
        let bytes = unsafe { input(data, length) }.ok_or(Error::InvalidArgument)?;
        if opcode_count != OPCODE_COUNT {
            return Err(Error::InvalidArgument);
        }
        let counts = unsafe { input(operand_counts, opcode_count as u64) }.ok_or(Error::InvalidArgument)?;
        instruction(bytes, counts, token_offset)
    })();
    unsafe { publish(out, result) }
}

/// Bounds one standalone operand in tokens [token_offset, token_end) of `data`.
#[no_mangle]
pub unsafe extern "C" fn dxmt_dxbc_operand_parse(
    data: *const u8, length: u64, token_offset: u32, token_end: u32, out: *mut u32,
) -> u32 {
    let result = (|| {
        let bytes = unsafe { input(data, length) }.ok_or(Error::InvalidArgument)?;
        if token_end as u64 * 4 > bytes.len() as u64 || token_offset > token_end {
            return Err(Error::InvalidArgument);
        }
        let mut cursor = Cursor { bytes, position: token_offset, end: token_end };
        cursor.operand(0)?;
        Ok(cursor.position)
    })();
    unsafe { publish(out, result) }
}

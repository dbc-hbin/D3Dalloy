// Included only by the Metal4 pipeline bridge. The returned compiler remains a
// real MTL4Compiler for MetalFX. Optional harvesting uses a separate compiler so
// flushing never races an unobserved MetalFX compile or blocks ordinary linking.
#include <objc/runtime.h>
#include <CommonCrypto/CommonDigest.h>
#include <mach-o/loader.h>
#include <dlfcn.h>
#include <sys/sysctl.h>
#include <sys/stat.h>
#include <sys/file.h>
#include <fcntl.h>
#include <unistd.h>

static char d3dalloy_archive_key;
static NSHashTable *d3dalloy_archive_states;
static BOOL d3dalloy_archive_enabled = YES;
static const NSUInteger kArchiveFiles = 16;
static const uint64_t kArchiveBytes = 256ull * 1024 * 1024;
static const uint64_t kArchiveShardBytes = 64ull * 1024 * 1024;

static BOOL archive_trace_enabled(void) {
  const char *value = getenv("D3DALLOY_METAL4_ARCHIVE_TRACE");
  return value && *value && strcmp(value,"0");
}
static void archive_trace(const char *event, uint64_t count) {
  if (archive_trace_enabled()) fprintf(stderr,"D3DALLOY_METAL4_ARCHIVE event=%s count=%llu\n",event,(unsigned long long)count);
}
static NSString *archive_namespace(id<MTLDevice> device) {
  const char *enabled = getenv("D3DALLOY_METAL4_ARCHIVE");
  if (enabled && !strcmp(enabled,"0")) return nil;
  Dl_info image;
  if (!dladdr((const void *)&archive_namespace,&image)) return nil;
  const struct mach_header_64 *header = (const struct mach_header_64 *)image.dli_fbase;
  if (header->magic != MH_MAGIC_64) return nil;
  const uint8_t *cursor = (const uint8_t *)(header+1), *end = cursor+header->sizeofcmds;
  NSData *uuid = nil;
  for (uint32_t i=0;i<header->ncmds;++i) {
    if ((size_t)(end-cursor)<sizeof(struct load_command)) return nil;
    const struct load_command *command = (const struct load_command *)cursor;
    if (command->cmdsize<sizeof(*command) || command->cmdsize>(size_t)(end-cursor)) return nil;
    if (command->cmd==LC_UUID && command->cmdsize>=sizeof(struct uuid_command)) {
      uuid=[NSData dataWithBytes:((const struct uuid_command*)command)->uuid length:16];break;
    }
    cursor+=command->cmdsize;
  }
  char build[128]={0};size_t buildSize=sizeof(build);
  if (!uuid || sysctlbyname("kern.osversion",build,&buildSize,NULL,0) || !memchr(build,0,sizeof(build))) return nil;
  NSMutableData *identity=[NSMutableData dataWithData:uuid];
  uint64_t registry=device.registryID;[identity appendBytes:&registry length:sizeof(registry)];
  [identity appendBytes:build length:strlen(build)+1];
  // Validation binaries and different prefixes never collide with a normal run.
  const char *variables[]={"WINEPREFIX","MTL_DEBUG_LAYER","MTL_SHADER_VALIDATION","MTL_SHADER_VALIDATION_ENABLE_ERROR_REPORTING"};
  for (unsigned i=0;i<sizeof(variables)/sizeof(variables[0]);++i) {
    const char *value=getenv(variables[i]);if (!value) value="";
    [identity appendBytes:value length:strlen(value)+1];
  }
  unsigned char digest[CC_SHA256_DIGEST_LENGTH];CC_SHA256(identity.bytes,(CC_LONG)identity.length,digest);
  char hex[2*CC_SHA256_DIGEST_LENGTH+1];
  for (unsigned i=0;i<CC_SHA256_DIGEST_LENGTH;++i) snprintf(hex+2*i,3,"%02x",digest[i]);
  NSString *root=nil;const char *override=getenv("D3DALLOY_METAL4_ARCHIVE_DIR");
  if (override && *override) {
    root=[NSString stringWithUTF8String:override];if (![root isAbsolutePath]) return nil;
  } else {
    char cache[PATH_MAX];size_t size=confstr(_CS_DARWIN_USER_CACHE_DIR,cache,sizeof(cache));
    if (!size || size>sizeof(cache)) return nil;
    root=[[NSString stringWithUTF8String:cache] stringByAppendingPathComponent:@"D3Dalloy/Metal4Archives-v1"];
  }
  return [root stringByAppendingPathComponent:[NSString stringWithUTF8String:hex]];
}

// Caller holds the directory lock. Ignore symlinks, foreign filenames and
// oversized entries; publication only renames a private UUID temporary file.
static NSArray *archive_files(NSString *directory) {
  NSMutableArray *files=[NSMutableArray array];
  for (NSString *name in [[NSFileManager defaultManager] contentsOfDirectoryAtPath:directory error:nil]) {
    if (![name hasPrefix:@"pso-"] || ![name hasSuffix:@".mtl4archive"]) continue;
    NSString *path=[directory stringByAppendingPathComponent:name];struct stat s;
    if (lstat(path.fileSystemRepresentation,&s) || !S_ISREG(s.st_mode) || s.st_size<0) continue;
    [files addObject:@{@"path":path,@"bytes":@(s.st_size),@"time":@(s.st_mtimespec.tv_sec),@"nanos":@(s.st_mtimespec.tv_nsec)}];
  }
  [files sortUsingComparator:^NSComparisonResult(NSDictionary *a,NSDictionary *b) {
    NSComparisonResult order=[b[@"time"] compare:a[@"time"]];
    if (order==NSOrderedSame) order=[b[@"nanos"] compare:a[@"nanos"]];
    return order==NSOrderedSame ? [a[@"path"] compare:b[@"path"]] : order;
  }];
  return files;
}
static int archive_lock(NSString *directory) {
  if (!directory) return -1;
  struct stat s;
  if (lstat(directory.fileSystemRepresentation,&s) || !S_ISDIR(s.st_mode)) return -1;
  int fd=open([[directory stringByAppendingPathComponent:@".lock"] fileSystemRepresentation],
      O_RDWR|O_CREAT|O_CLOEXEC|O_NOFOLLOW,0600);
  if (fd>=0 && flock(fd,LOCK_EX)) {close(fd);return -1;}
  return fd;
}

@interface D3DalloyPipelineArchive : NSObject {
  id<MTLDevice> _device;
  NSString *_directory;
  id<MTL4Compiler> _capture;
  id<MTL4PipelineDataSetSerializer> _serializer;
  NSArray *_archives;
  dispatch_queue_t _writer;
  NSLock *_publication;
  uint64_t _generation;
  uint64_t _archiveBytes;
  NSUInteger _active, _pending;
  BOOL _enabled, _resetCapture, _flushing, _scheduled, _captureUnavailable;
}
- (instancetype)initWithDevice:(id<MTLDevice>)device;
- (NSArray *)copyArchives;
- (id<MTL4Compiler>)beginCapture:(uint64_t *)generation;
- (void)endCapture:(uint64_t)generation success:(BOOL)success;
- (void)flush;
- (void)control:(uint32_t)flags;
+ (void)controlAll:(uint32_t)flags;
@end

@implementation D3DalloyPipelineArchive
- (instancetype)initWithDevice:(id<MTLDevice>)device {
  if (!(self=[super init])) return nil;
  _directory=[archive_namespace(device) copy];
  if (!_directory || ![[NSFileManager defaultManager] createDirectoryAtPath:_directory withIntermediateDirectories:YES
      attributes:@{NSFilePosixPermissions:@0700} error:nil]) { [self release];return nil; }
  _device=[device retain];_archives=[[NSArray alloc] init];
  _writer=dispatch_queue_create("d3dalloy.metal4.archive",DISPATCH_QUEUE_SERIAL);_resetCapture=YES;
  _publication=[[NSLock alloc] init];
  @synchronized([D3DalloyPipelineArchive class]) {
    _enabled=d3dalloy_archive_enabled;
    if (!d3dalloy_archive_states) d3dalloy_archive_states=[[NSHashTable weakObjectsHashTable] retain];
    [d3dalloy_archive_states addObject:self];
    if (_enabled) {
      int fd=archive_lock(_directory);
      if (fd>=0) {
        NSMutableArray *loaded=[NSMutableArray array];uint64_t bytes=0;
        for (NSDictionary *file in archive_files(_directory)) {
          uint64_t size=[file[@"bytes"] unsignedLongLongValue];
          if (!size || size>kArchiveShardBytes) continue;
          if (loaded.count>=kArchiveFiles || bytes+size>kArchiveBytes) break;
          NSError *error=nil;id<MTL4Archive> archive=[_device newArchiveWithURL:[NSURL fileURLWithPath:file[@"path"]] error:&error];
          if (archive) {[loaded addObject:archive];[archive release];bytes+=size;}
          else {
            archive_trace("invalid_file",1);
            if (archive_trace_enabled()) fprintf(stderr,"D3DALLOY_METAL4_ARCHIVE load_error=%s\n",error.localizedDescription.UTF8String);
          }
        }
        [_archives release];_archives=[loaded copy];_archiveBytes=bytes;close(fd);
      }
    }
  }
  archive_trace("load",_archives.count);
  return self;
}
- (NSArray *)copyArchives { @synchronized(self) { return _enabled ? [_archives copy] : nil; } }
- (id<MTL4Compiler>)beginCapture:(uint64_t *)generation {
  @synchronized(self) {
    // While disk work owns the serializer, use the ordinary caller compiler.
    // No new compilation waits for archive serialization or another key.
    if (!_enabled || _captureUnavailable || _flushing || _active+_pending>=64) return nil;
    if (_resetCapture) {
      if (_active) return nil;
      [_capture release];_capture=nil;[_serializer release];_serializer=nil;
      MTL4PipelineDataSetSerializerDescriptor *data=[[MTL4PipelineDataSetSerializerDescriptor alloc] init];
      data.configuration=MTL4PipelineDataSetSerializerConfigurationCaptureBinaries;
      _serializer=[_device newPipelineDataSetSerializerWithDescriptor:data];[data release];
      if (!_serializer) return nil;
      MTL4CompilerDescriptor *descriptor=[[MTL4CompilerDescriptor alloc] init];descriptor.pipelineDataSetSerializer=_serializer;
      _capture=[_device newCompilerWithDescriptor:descriptor error:nil];[descriptor release];
      if (!_capture) return nil;
      _resetCapture=NO;
    }
    ++_active;*generation=_generation;return [_capture retain];
  }
}
- (void)scheduleFlush {
  if (_scheduled || !_pending) return;
  _scheduled=YES;
  dispatch_after(dispatch_time(DISPATCH_TIME_NOW,NSEC_PER_SEC),_writer,^{ @autoreleasepool {
    @synchronized(self) { self->_scheduled=NO; }
    [self flushIdle];
  }});
}
- (void)endCapture:(uint64_t)generation success:(BOOL)success {
  @synchronized(self) {
    if (_active) --_active;
    if (success && generation==_generation && _enabled) ++_pending;
    [self scheduleFlush];
  }
}
- (void)flushIdle {
  id<MTL4PipelineDataSetSerializer> serializer=nil;uint64_t generation=0;
  @synchronized(self) {
    if (!_enabled || !_pending || _flushing) return;
    if (_active) { [self scheduleFlush];return; }
    _flushing=YES;_pending=0;generation=_generation;serializer=[_serializer retain];
  }
  NSString *token=[[NSUUID UUID] UUIDString];
  NSString *temporary=[_directory stringByAppendingPathComponent:[@".tmp-" stringByAppendingString:token]];
  @try {
    NSError *error=nil;BOOL saved=[serializer serializeAsArchiveAndFlushToURL:[NSURL fileURLWithPath:temporary] error:&error];
    if (!saved && archive_trace_enabled()) fprintf(stderr,"D3DALLOY_METAL4_ARCHIVE serialize_error=%s\n",error.localizedDescription.UTF8String);
    struct stat s;BOOL regular=saved && !lstat(temporary.fileSystemRepresentation,&s) && S_ISREG(s.st_mode) &&
        s.st_size>0 && (uint64_t)s.st_size<=kArchiveShardBytes;
    // A serializer can return bytes that this same device cannot load (notably
    // instrumented pipelines on some OS builds). Never publish them or repeat
    // failed capture indefinitely, and never turn shader validation off.
    NSError *loadError=nil;
    id<MTL4Archive> archive=regular ? [_device newArchiveWithURL:[NSURL fileURLWithPath:temporary] error:&loadError] : nil;
    if (regular && !archive) {
      archive_trace("incompatible_capture",1);
      if (archive_trace_enabled()) fprintf(stderr,"D3DALLOY_METAL4_ARCHIVE capture_load_error=%s\n",loadError.localizedDescription.UTF8String);
      @synchronized(self) {if (generation==_generation) _captureUnavailable=YES;}
    }
    // Directory I/O never owns the short metadata monitor used by compilers.
    // Clear retires the generation first, then joins this publication lock.
    [_publication lock];
    @try {
      BOOL current=NO;
      @synchronized(self) {current=_enabled && generation==_generation;}
      if (archive && current) {
        int fd=archive_lock(_directory);
        if (fd>=0) {
          NSString *path=[_directory stringByAppendingPathComponent:[NSString stringWithFormat:@"pso-%@.mtl4archive",token]];
          if (!rename(temporary.fileSystemRepresentation,path.fileSystemRepresentation)) {
            uint64_t bytes=0;NSUInteger count=0;
            for (NSDictionary *file in archive_files(_directory)) {
              uint64_t size=[file[@"bytes"] unsignedLongLongValue];
              if (!size || size>kArchiveShardBytes || count>=kArchiveFiles || bytes+size>kArchiveBytes)
                unlink([file[@"path"] fileSystemRepresentation]);
              else {++count;bytes+=size;}
            }
            @synchronized(self) {
              if (_enabled && generation==_generation) {
                NSMutableArray *loaded=[NSMutableArray arrayWithObject:archive];
                // Bound live archive owners as well as directory contents. A
                // batch reset is cheaper than mutating per-pipeline metadata.
                if (_archives.count<kArchiveFiles && _archiveBytes+(uint64_t)s.st_size<=kArchiveBytes)
                  [loaded addObjectsFromArray:_archives];
                else _archiveBytes=0;
                _archiveBytes+=(uint64_t)s.st_size;
                [_archives release];_archives=[loaded copy];
              }
            }
            archive_trace("publish",s.st_size);
          } else archive_trace("write_failure",1);
          close(fd);
        } else archive_trace("lock_failure",1);
      } else archive_trace(archive?"retired_publish":"serialize_failure",1);
    } @finally {
      [_publication unlock];[archive release];
    }
  } @catch (NSException *exception) { archive_trace("serialize_exception",1); }
  @finally {
    unlink(temporary.fileSystemRepresentation);[serializer release];
    @synchronized(self) { _flushing=NO;_resetCapture=YES;[self scheduleFlush]; }
  }
}
- (void)flush {
  // Explicit lifecycle boundary only. Ordinary pipeline callers never wait for I/O.
  if (_writer) dispatch_sync(_writer,^{ @autoreleasepool { [self flushIdle]; }});
}
- (void)control:(uint32_t)flags {
  @synchronized(self) {
    if (flags & (1u|4u)) {++_generation;_pending=0;_resetCapture=YES;_archiveBytes=0;[_archives release];_archives=[[NSArray alloc] init];}
    if (flags & 1u) _enabled=NO;
    if (flags & 2u) _enabled=YES;
    archive_trace("control",flags);
  }
  if (flags & 4u) {
    [_publication lock];
    @try {
      int fd=archive_lock(_directory);
      if (fd>=0) {for (NSDictionary *file in archive_files(_directory)) unlink([file[@"path"] fileSystemRepresentation]);close(fd);}
    } @finally {[_publication unlock];}
  }
}
+ (void)controlAll:(uint32_t)flags {
  @synchronized(self) {
    if (flags & 1u) d3dalloy_archive_enabled=NO;
    if (flags & 2u) d3dalloy_archive_enabled=YES;
    for (D3DalloyPipelineArchive *state in [d3dalloy_archive_states allObjects]) [state control:flags];
  }
}
- (void)dealloc {
  [_capture release];[_serializer release];[_archives release];[_device release];[_directory release];[_publication release];
  if (_writer) dispatch_release(_writer);
  [super dealloc];
}
@end

static D3DalloyPipelineArchive *pipeline_archive(id<MTL4Compiler> compiler) {
  return objc_getAssociatedObject(compiler,&d3dalloy_archive_key);
}
static id archive_pipeline(id<MTL4Compiler> compiler, MTL4PipelineDescriptor *descriptor,
                          MTL4PipelineStageDynamicLinkingDescriptor *dynamic, BOOL compute, NSError **error) {
  D3DalloyPipelineArchive *state=pipeline_archive(compiler);
  NSArray *archives=[state copyArchives];
  // An explicit archive factory gives a real hit/miss, rather than guessing from
  // elapsed compile time. Incompatible/corrupt/missing entries fall back below.
  for (id<MTL4Archive> archive in archives) {
    @try {
      id pipeline=compute ? (id)[archive newComputePipelineStateWithDescriptor:(MTL4ComputePipelineDescriptor*)descriptor
          dynamicLinkingDescriptor:dynamic error:nil] : [archive newRenderPipelineStateWithDescriptor:descriptor error:nil];
      if (pipeline) {archive_trace("hit",1);[archives release];return pipeline;}
    } @catch (NSException *exception) {archive_trace("lookup_exception",1);}
  }
  uint64_t generation=0;id<MTL4Compiler> capture=nil;
  @try {capture=[state beginCapture:&generation];}
  @catch (NSException *exception) {archive_trace("capture_exception",1);}
  id<MTL4Compiler> selected=capture?capture:compiler;
  MTL4CompilerTaskOptions *options=nil;
  if (archives.count) {options=[[MTL4CompilerTaskOptions alloc] init];options.lookupArchives=archives;}
  id result=nil;BOOL captured=NO;
  @try {
    archive_trace("compile",1);
    @try {
      result=compute ? (id)[selected newComputePipelineStateWithDescriptor:(MTL4ComputePipelineDescriptor*)descriptor
          dynamicLinkingDescriptor:dynamic compilerTaskOptions:options error:error]
          : [selected newRenderPipelineStateWithDescriptor:descriptor compilerTaskOptions:options error:error];
      captured=capture && result;
    } @catch (NSException *exception) {
      if (!capture && !options) @throw;
      archive_trace("optional_compile_exception",1);
    }
    if (!result && (options || capture)) {
      // A bad optional archive must not turn a valid pipeline into a failure.
      archive_trace("retry_without_archive",1);
      if (error) *error=nil;
      result=compute ? (id)[compiler newComputePipelineStateWithDescriptor:(MTL4ComputePipelineDescriptor*)descriptor
          dynamicLinkingDescriptor:dynamic compilerTaskOptions:nil error:error]
          : [compiler newRenderPipelineStateWithDescriptor:descriptor compilerTaskOptions:nil error:error];
    }
  } @finally {
    if (capture) [state endCapture:generation success:captured];
    [capture release];[options release];[archives release];
  }
  return result;
}

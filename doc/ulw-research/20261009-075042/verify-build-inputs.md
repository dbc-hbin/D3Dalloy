# Recorded sources and executable PE sections

Executed `python3 doc/ulw-research/20261009-075042/verify_build_inputs.py`.

```json
{
  "source_entries_new": 568,
  "source_entries_old": 568,
  "source_differences": [],
  "PE_comparison": {
    "d3d12.dll": {
      "executable_sections_equal": false,
      "sections": [
        {
          ".text": {
            "rva": 16384,
            "virtual_bytes": 4757926,
            "sha256": "da2d978f94da46992b85aa54fb715df92a06b25b2070d7ee6b0c91eb03d8093c"
          },
          ".hexpthk": {
            "rva": 4784128,
            "virtual_bytes": 272,
            "sha256": "261ad1ce2bd10b93244ea5d8f9b422878173beb61f9950617b2c4fc12a3e5b8f"
          }
        },
        {
          ".text": {
            "rva": 16384,
            "virtual_bytes": 4757926,
            "sha256": "0a57d0a1090f3363a8a747a3f2aa08a17ea0dcfba95839d1fd26db40c799ef90"
          },
          ".hexpthk": {
            "rva": 4784128,
            "virtual_bytes": 272,
            "sha256": "261ad1ce2bd10b93244ea5d8f9b422878173beb61f9950617b2c4fc12a3e5b8f"
          }
        }
      ]
    },
    "d3d12core.dll": {
      "executable_sections_equal": true,
      "sections": [
        {
          ".text": {
            "rva": 16384,
            "virtual_bytes": 488950,
            "sha256": "165d9cbc27026b332dc08a74434a825d8a949e4970b2feb43a4016a55976d54c"
          },
          ".hexpthk": {
            "rva": 507904,
            "virtual_bytes": 64,
            "sha256": "d61da1928049b07c9e3d3bbe9838530368b37e152a80b235e14c5ed5aeb0251c"
          }
        },
        {
          ".text": {
            "rva": 16384,
            "virtual_bytes": 488950,
            "sha256": "165d9cbc27026b332dc08a74434a825d8a949e4970b2feb43a4016a55976d54c"
          },
          ".hexpthk": {
            "rva": 507904,
            "virtual_bytes": 64,
            "sha256": "d61da1928049b07c9e3d3bbe9838530368b37e152a80b235e14c5ed5aeb0251c"
          }
        }
      ]
    },
    "nvngx.dll": {
      "executable_sections_equal": true,
      "sections": [
        {
          ".text": {
            "rva": 16384,
            "virtual_bytes": 1317222,
            "sha256": "82e76dae717e209be6adde173c3a03dda8c968f46d42446cd19956d2f207b99b"
          },
          ".hexpthk": {
            "rva": 1343488,
            "virtual_bytes": 528,
            "sha256": "b5c622c83ee02b6b5acd6cf2f6939ef1e82d8de3eba68923476dff00e24ec6a3"
          }
        },
        {
          ".text": {
            "rva": 16384,
            "virtual_bytes": 1317222,
            "sha256": "82e76dae717e209be6adde173c3a03dda8c968f46d42446cd19956d2f207b99b"
          },
          ".hexpthk": {
            "rva": 1343488,
            "virtual_bytes": 528,
            "sha256": "b5c622c83ee02b6b5acd6cf2f6939ef1e82d8de3eba68923476dff00e24ec6a3"
          }
        }
      ]
    },
    "dxgi.dll": {
      "executable_sections_equal": true,
      "sections": [
        {
          ".text": {
            "rva": 16384,
            "virtual_bytes": 1576214,
            "sha256": "e6fd43443258394d3ab01c26cde0f0e05b64e0893345779d4708f4566d7af859"
          },
          ".hexpthk": {
            "rva": 1605632,
            "virtual_bytes": 240,
            "sha256": "046387f8011992180b4be2d2ccb9bd3b2d548918b75b220696c7abf62e0d67c2"
          }
        },
        {
          ".text": {
            "rva": 16384,
            "virtual_bytes": 1576214,
            "sha256": "e6fd43443258394d3ab01c26cde0f0e05b64e0893345779d4708f4566d7af859"
          },
          ".hexpthk": {
            "rva": 1605632,
            "virtual_bytes": 240,
            "sha256": "046387f8011992180b4be2d2ccb9bd3b2d548918b75b220696c7abf62e0d67c2"
          }
        }
      ]
    },
    "d3d11.dll": {
      "executable_sections_equal": true,
      "sections": [
        {
          ".text": {
            "rva": 16384,
            "virtual_bytes": 3617926,
            "sha256": "b326d1ad39b4901331e24fc5d3da1e6cc2d5781d3da840041a1543db0c667ffb"
          },
          ".hexpthk": {
            "rva": 3637248,
            "virtual_bytes": 272,
            "sha256": "e346c0096fbc7dc6b5f424a94f337f5613fb974de6aed6caf65ec86d9174a7e9"
          }
        },
        {
          ".text": {
            "rva": 16384,
            "virtual_bytes": 3617926,
            "sha256": "b326d1ad39b4901331e24fc5d3da1e6cc2d5781d3da840041a1543db0c667ffb"
          },
          ".hexpthk": {
            "rva": 3637248,
            "virtual_bytes": 272,
            "sha256": "e346c0096fbc7dc6b5f424a94f337f5613fb974de6aed6caf65ec86d9174a7e9"
          }
        }
      ]
    },
    "xtajit64.dll": {
      "executable_sections_equal": false,
      "sections": [
        {
          ".text": {
            "rva": 16384,
            "virtual_bytes": 2205670,
            "sha256": "c4709d48a1a64aeefde532d3aca93e0dd9f3a3cef812f557aa5513b856c44a73"
          },
          ".hexpthk": {
            "rva": 2228224,
            "virtual_bytes": 272,
            "sha256": "38fb3720d8b83808d41546936a50ca3194d893f7ea32639f878ef6ea8f4bf662"
          }
        },
        {
          ".text": {
            "rva": 16384,
            "virtual_bytes": 2205670,
            "sha256": "70b9661d20f029ee756dbcadadceebe40ddfd48c7979a971b532dfb95082ef5c"
          },
          ".hexpthk": {
            "rva": 2228224,
            "virtual_bytes": 272,
            "sha256": "38fb3720d8b83808d41546936a50ca3194d893f7ea32639f878ef6ea8f4bf662"
          }
        }
      ]
    }
  },
  "limit": "Equal code bytes do not imply all data/build inputs or runtime settings are equal; differing code bytes do not by themselves establish a regression."
}


Wall time: 0.10 seconds
```

# Smile2Unlock

A Windows-based facial recognition login system that integrates with Windows Credential Provider for biometric authentication. (Temporary)

## Components

### FaceRecognizer
Command-line tool for facial feature extraction and recognition processing.

### SampleV2CredentialProvider
Windows Credential Provider DLL that integrates facial recognition into Windows login flow.

## Requirements

- Windows 10 or later
- Visual Studio 2022 or later
- xmake build system
- Dependencies (automatically managed via xmake):
  - SeetaFace6
  - Crypto++
  - cxxopts

## Building

```bash
xmake build
```

## License

This project is licensed under the **MIT License** - see [LICENSE](LICENSE) file for details.

## Third-party Licenses

This project uses several third-party libraries. See [NOTICE/THIRD-PARTY-NOTICES.md](NOTICE/THIRD-PARTY-NOTICES.md) for complete attribution and license information.

### Dependencies

- **SeetaFace6** - BSD-2-Clause
- **Crypto++** - Boost Software License 1.0
- **cxxopts** - MIT
- **INIcpp** - MIT
- **Windows-classic-samples** - MIT

## Contributing

Contributions are welcome! Please ensure any new third-party dependencies are properly documented in THIRD-PARTY-NOTICES.md.

## Security Notice

This is a biometric authentication system that handles sensitive Windows security credentials. Please review the code carefully and perform appropriate security testing before deploying in production environments.

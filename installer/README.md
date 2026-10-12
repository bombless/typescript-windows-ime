# Windows MSI packaging

The Windows CI workflow builds the TypeScript core and native C++ TSF/Host, stages production npm dependencies, and produces an MSI artifact.

The MSI is per-machine because TSF COM registration requires elevation. It registers the native TSF DLL using regsvr32 and checks that the Chinese language profile exists. Startup entries launch the native Host hidden and start the TypeScript client via a hidden VBScript wrapper.

The native C++ DLL under native/install is registered as the TSF input method. The Host is not registered as an input method; it is only a background named-pipe server.

Build on Windows with Node.js 20+, Visual Studio 2022 C++ x64 tools, and WiX Toolset v4. The GitHub Actions workflow builds and uploads the MSI as an artifact.

// npm run native:build: builds ory1-vsthost.exe and vstbridge.node into native/bin with Visual Studio's own CMake and Ninja.
// Needs Visual Studio (or its Build Tools) with the "Desktop development with C++" workload.
const { execFileSync } = require('child_process');
const fs = require('fs');
const path = require('path');

if (process.platform !== 'win32') { console.log('VST3 hosting is Windows-only for now: nothing to build.'); process.exit(0); }

const vswhere = path.join(process.env['ProgramFiles(x86)'], 'Microsoft Visual Studio', 'Installer', 'vswhere.exe');
const vs = execFileSync(vswhere, ['-latest', '-products', '*', '-requires', 'Microsoft.VisualStudio.Component.VC.Tools.x86.x64', '-property', 'installationPath'], { encoding: 'utf8' }).trim().split(/\r?\n/)[0];
if (!vs) { console.error('No Visual Studio with the C++ tools was found. Add the "Desktop development with C++" workload.'); process.exit(1); }
const vcvars = path.join(vs, 'VC', 'Auxiliary', 'Build', 'vcvars64.bat');
const cmakeDir = path.join(vs, 'Common7', 'IDE', 'CommonExtensions', 'Microsoft', 'CMake');
const cmake = path.join(cmakeDir, 'CMake', 'bin', 'cmake.exe');
const ninja = path.join(cmakeDir, 'Ninja', 'ninja.exe');
const src = __dirname, build = path.join(__dirname, 'build');
if (!fs.existsSync(path.join(src, 'vst3sdk', 'CMakeLists.txt'))) { console.error('native/vst3sdk is missing: run  git submodule update --init --recursive'); process.exit(1); }

const q = s => `"${s}"`;
// --test-plugins builds the SDK's mda plugins (synths and effects) into native/build-sdk/VST3/Release, for testing
const testPlugins = process.argv.includes('--test-plugins');
const steps = testPlugins ? [
  `call ${q(vcvars)} >nul`,
  `${q(cmake)} -S ${q(path.join(src, 'vst3sdk'))} -B ${q(path.join(src, 'build-sdk'))} -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_MAKE_PROGRAM=${q(ninja)} -DSMTG_CREATE_PLUGIN_LINK=OFF -DSMTG_ENABLE_VSTGUI_SUPPORT=OFF -DSMTG_ENABLE_VST3_HOSTING_EXAMPLES=OFF -DSMTG_RUN_VST_VALIDATOR=OFF`,
  `${q(cmake)} --build ${q(path.join(src, 'build-sdk'))} --target mda-vst3`,
] : [
  `call ${q(vcvars)} >nul`,
  `${q(cmake)} -S ${q(src)} -B ${q(build)} -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_MAKE_PROGRAM=${q(ninja)}`,
  `${q(cmake)} --build ${q(build)} --config Release`,
];
const bat = path.join(build, 'run.bat');
fs.mkdirSync(build, { recursive: true });
fs.writeFileSync(bat, '@echo off\r\n' + steps.map(s => s + ' || exit /b 1').join('\r\n') + '\r\n');
try { execFileSync('cmd.exe', ['/d', '/c', bat], { stdio: 'inherit' }); }
catch (e) { process.exit(1); }
console.log(testPlugins ? 'Built native/build-sdk/VST3/Release/mda-vst3.vst3' : 'Built native/bin/ory1-vsthost.exe and native/bin/vstbridge.node');

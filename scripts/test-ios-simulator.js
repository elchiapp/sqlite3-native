#!/usr/bin/env node

const fs = require('fs')
const path = require('path')
const { spawnSync } = require('child_process')

const root = path.resolve(__dirname, '..')
const arch = process.env.IOS_SIMULATOR_ARCH || (process.arch === 'arm64' ? 'arm64' : 'x64')
const target = `ios-${arch}-simulator`
const sdkArch = arch === 'x64' ? 'x86_64' : arch
const prebuild = path.join(root, 'prebuilds', target, 'sqlite3-native.bare')
const runtimePackage = `bare-runtime-${target}`
const runtimeVersion = process.env.BARE_IOS_SIMULATOR_RUNTIME_VERSION || '^1.28.5'
const runtimeDir =
  process.env.BARE_IOS_SIMULATOR_RUNTIME_DIR ||
  path.join(root, 'build', 'ios-simulator-runtime', arch)

try {
  process.exitCode = main()
} catch (err) {
  console.error(err.message)
  process.exitCode = 1
}

function main() {
  if (process.platform !== 'darwin') {
    throw new Error('test:ios-simulator requires macOS and Xcode.')
  }

  if (arch !== 'arm64' && arch !== 'x64') {
    throw new Error('IOS_SIMULATOR_ARCH must be "arm64" or "x64".')
  }

  const bare = ensureBareRuntime()
  ensurePrebuild()

  const device = selectSimulator()
  let bootedByScript = false

  try {
    if (device.state !== 'Booted') {
      log(`Booting ${device.name} (${device.udid})`)
      run('xcrun', ['simctl', 'boot', device.udid])
      bootedByScript = true
      run('xcrun', ['simctl', 'bootstatus', device.udid, '-b'])
    }

    log(`Running test.js on ${device.name} (${target})`)
    return status('xcrun', ['simctl', 'spawn', device.udid, bare, path.join(root, 'test.js')])
  } finally {
    if (bootedByScript && process.env.IOS_SIMULATOR_KEEP_BOOTED !== '1') {
      log(`Shutting down ${device.name} (${device.udid})`)
      status('xcrun', ['simctl', 'shutdown', device.udid])
    }
  }
}

function ensureBareRuntime() {
  if (process.env.BARE_IOS_SIMULATOR_BIN) {
    const bin = process.env.BARE_IOS_SIMULATOR_BIN
    if (!fs.existsSync(bin)) throw new Error(`Bare runtime not found: ${bin}`)
    return bin
  }

  const bin = path.join(runtimeDir, 'node_modules', runtimePackage, 'bin', 'bare')
  if (fs.existsSync(bin)) return bin

  fs.mkdirSync(runtimeDir, { recursive: true })

  const manifest = path.join(runtimeDir, 'package.json')
  if (!fs.existsSync(manifest)) {
    fs.writeFileSync(manifest, JSON.stringify({ private: true }, null, 2) + '\n')
  }

  log(`Installing ${runtimePackage}@${runtimeVersion}`)
  run('npm', [
    'install',
    '--prefix',
    runtimeDir,
    '--no-save',
    '--force',
    '--no-audit',
    '--no-fund',
    `${runtimePackage}@${runtimeVersion}`
  ])

  if (!fs.existsSync(bin)) throw new Error(`Bare runtime install did not create ${bin}`)
  return bin
}

function ensurePrebuild() {
  if (fs.existsSync(prebuild)) return

  const buildDir = path.join(root, 'build', target)

  log(`Building ${target} prebuild`)
  run('cmake', [
    '-S',
    root,
    '-B',
    buildDir,
    '-DCMAKE_SYSTEM_NAME=iOS',
    '-DCMAKE_OSX_SYSROOT=iphonesimulator',
    `-DCMAKE_OSX_ARCHITECTURES=${sdkArch}`,
    `-DCMAKE_INSTALL_PREFIX=${path.join(root, 'prebuilds')}`,
    '-DCMAKE_BUILD_TYPE=Release'
  ])
  run('cmake', ['--build', buildDir, '--config', 'Release'])
  run('cmake', ['--install', buildDir, '--config', 'Release'])

  if (!fs.existsSync(prebuild)) throw new Error(`Prebuild was not created: ${prebuild}`)
}

function selectSimulator() {
  const listing = JSON.parse(capture('xcrun', ['simctl', 'list', 'devices', 'available', '-j']))
  const requested = process.env.IOS_SIMULATOR_ID || process.env.IOS_SIMULATOR_DEVICE
  const devices = []

  for (const [runtime, runtimeDevices] of Object.entries(listing.devices || {})) {
    if (!runtime.includes('SimRuntime.iOS')) continue
    for (const device of runtimeDevices) {
      if (device.isAvailable === false) continue
      devices.push(device)
    }
  }

  if (process.env.IOS_SIMULATOR_ID) {
    const device = devices.find((candidate) => candidate.udid === process.env.IOS_SIMULATOR_ID)
    if (!device) {
      throw new Error(`No available iOS simulator matches ${process.env.IOS_SIMULATOR_ID}`)
    }
    return device
  }

  const booted = devices.find(
    (device) => device.state === 'Booted' && matchesDevice(device, requested)
  )
  if (booted) return booted

  const device = devices.find((candidate) => matchesDevice(candidate, requested))
  if (device) return device

  throw new Error('No available iOS simulator found.')
}

function matchesDevice(device, requested) {
  if (!requested) return true

  const value = requested.toLowerCase()
  return device.udid === requested || device.name.toLowerCase().includes(value)
}

function capture(command, args) {
  const result = spawnSync(command, args, {
    cwd: root,
    encoding: 'utf8'
  })

  if (result.error) throw result.error
  if (result.status !== 0) {
    if (result.stderr) process.stderr.write(result.stderr)
    throw new Error(`${command} ${args.join(' ')} exited with ${result.status}`)
  }

  return result.stdout
}

function run(command, args) {
  const code = status(command, args)
  if (code !== 0) throw new Error(`${command} ${args.join(' ')} exited with ${code}`)
}

function status(command, args) {
  log(`$ ${command} ${args.join(' ')}`)

  const result = spawnSync(command, args, {
    cwd: root,
    stdio: 'inherit'
  })

  if (result.error) throw result.error
  if (result.status !== null) return result.status

  console.error(`${command} ${args.join(' ')} exited from signal ${result.signal}`)
  return 1
}

function log(message) {
  console.error(`[ios-simulator] ${message}`)
}

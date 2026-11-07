#!/usr/bin/env node
import { appendFileSync, closeSync, existsSync, fsyncSync, openSync } from 'node:fs'
import { isAbsolute, resolve } from 'node:path'
import { execFileSync } from 'node:child_process'
import { decode } from 'cborg'

const maximumOutputBytes = 65536
const maximumRecordBytes = 16384
const maximumDurationSeconds = 8 * 60 * 60
const sourceAddress = process.env.NAWSERVER_SOURCE_ADDRESS
if (!sourceAddress) throw new Error('Set NAWSERVER_SOURCE_ADDRESS to the local LAN IPv4 address')

function parseArguments(argv) {
  const values = new Map()
  for (let index = 0; index < argv.length; index += 2) {
    const key = argv[index]
    const value = argv[index + 1]
    if (!key?.startsWith('--') || value === undefined || value.startsWith('--')) {
      throw new Error('Usage: node tools/load/sample-memory.mjs --base http://DEVICE --out ABSOLUTE.jsonl --stop ABSOLUTE.stop --duration-seconds N [--interval-seconds 60]')
    }
    if (values.has(key)) throw new Error(`Duplicate option ${key}`)
    values.set(key, value)
  }

  const base = values.get('--base')
  const outputPath = values.get('--out')
  const stopPath = values.get('--stop')
  const durationSeconds = Number(values.get('--duration-seconds'))
  const intervalSeconds = Number(values.get('--interval-seconds') ?? '60')
  const baseURL = new URL(base)
  if (baseURL.protocol !== 'http:' || !baseURL.hostname || baseURL.pathname !== '/' || baseURL.search || baseURL.hash || baseURL.username || baseURL.password) {
    throw new Error('--base must be a plain HTTP origin without credentials or a path')
  }
  if (baseURL.port === '443') throw new Error('Port 443 is forbidden for project traffic')
  if (!isAbsolute(outputPath ?? '') || !isAbsolute(stopPath ?? '')) throw new Error('--out and --stop must be absolute paths')
  if (!Number.isInteger(durationSeconds) || durationSeconds < 1 || durationSeconds > maximumDurationSeconds) {
    throw new Error('--duration-seconds must be an integer from 1 to 28800')
  }
  if (!Number.isInteger(intervalSeconds) || intervalSeconds < 10 || intervalSeconds > 3600) {
    throw new Error('--interval-seconds must be an integer from 10 to 3600')
  }
  const allowed = new Set(['--base', '--out', '--stop', '--duration-seconds', '--interval-seconds'])
  for (const key of values.keys()) if (!allowed.has(key)) throw new Error(`Unknown option ${key}`)
  return { base: baseURL.origin, outputPath: resolve(outputPath), stopPath: resolve(stopPath), durationSeconds, intervalSeconds }
}

function curlCBOR(base, endpoint) {
  const args = [
    '--noproxy', '*', '--interface', sourceAddress,
    '--fail', '--silent', '--show-error', '--max-time', '4',
    '--max-filesize', String(maximumOutputBytes), '--output', '-',
    `${base}${endpoint}`
  ]
  try {
    const body = execFileSync('curl.exe', args, { encoding: null, maxBuffer: maximumOutputBytes, windowsHide: true })
    if (body.length > maximumOutputBytes) throw new Error(`response exceeded ${maximumOutputBytes} bytes`)
    return decode(body)
  } catch (error) {
    throw new Error(`GET ${endpoint}: ${error.message}`, { cause: error })
  }
}

function requireMap(value, name) {
  if (value === null || Array.isArray(value) || typeof value !== 'object') throw new Error(`${name} is not a CBOR map`)
  return value
}

function firmwareIdentity(info) {
  const root = requireMap(info, 'info')
  if (root.source !== 'device') throw new Error('info source is not device')
  const firmware = requireMap(root.firmware, 'info.firmware')
  const details = requireMap(root.details, 'info.details')
  const build = requireMap(details.build, 'info.details.build')
  const elfSha256 = text(build.elf_sha256)
  if (!/^[0-9a-fA-F]{64}$/.test(elfSha256)) throw new Error('info.details.build.elf_sha256 is not a 64-character SHA-256')
  return {
    boot_id: text(root.boot_id),
    chip: text(requireMap(root.chip, 'info.chip').model),
    firmware_name: text(firmware.name),
    firmware_version: text(firmware.version),
    idf_version: text(firmware.idf_version),
    json_backend: text(firmware.json_backend),
    http_backend: text(firmware.http_backend),
    json_workspace_bytes: optionalFiniteNumber(firmware.json_workspace_bytes),
    mcp_output_bytes: optionalFiniteNumber(firmware.mcp_output_bytes),
    mcp_input_bytes: optionalFiniteNumber(firmware.mcp_input_bytes),
    build_date: text(firmware.build_date),
    build_time: text(firmware.build_time),
    elf_sha256: elfSha256.toLowerCase()
  }
}

function memorySample(summary) {
  const root = requireMap(summary, 'summary')
  if (root.source !== 'device' || typeof root.boot_id !== 'string') throw new Error('summary device identity missing')
  const data = requireMap(root.data, 'summary.data')
  if (!Array.isArray(data.memory) || data.memory.length === 0) throw new Error('summary memory array missing')
  const memoryRegions = data.memory.map((region, index) => {
    const item = requireMap(region, `summary.data.memory[${index}]`)
    return {
      id: text(item.id),
      total_bytes: finiteNumber(item.total_bytes),
      free_bytes: finiteNumber(item.free_bytes),
      min_free_bytes: finiteNumber(item.min_free_bytes),
      largest_block_bytes: finiteNumber(item.largest_block_bytes)
    }
  })
  const memory = memoryRegions.find(region => region.id === 'internal') ?? memoryRegions[0]
  const cpu = Array.isArray(data.core_busy_pct) ? data.core_busy_pct : []
  return {
    boot_id: root.boot_id,
    memory: memoryRegions,
    heap_free_bytes: finiteNumber(memory.free_bytes),
    heap_min_free_bytes: finiteNumber(memory.min_free_bytes),
    heap_largest_block_bytes: finiteNumber(memory.largest_block_bytes),
    core_busy_pct: cpu.map(finiteNumber)
  }
}

function taskSample(tasks) {
  const root = requireMap(tasks, 'tasks')
  if (root.source !== 'device' || typeof root.boot_id !== 'string' || root.boot_id.length === 0) throw new Error('tasks device identity missing')
  const data = requireMap(root.data, 'tasks.data')
  if (!Array.isArray(data.items)) throw new Error('tasks data.items missing')
  return { boot_id: root.boot_id, tasks: data.items.slice(0, 8).map((task, index) => {
    const item = requireMap(task, `tasks.data.items[${index}]`)
    return {
      name: text(item.name),
      stack_min_free_bytes: finiteNumber(item.stack_min_free_bytes)
    }
  }) }
}

function finiteNumber(value) {
  if (typeof value !== 'number' || !Number.isFinite(value)) throw new Error('expected finite CBOR number')
  return value
}

function text(value) {
  return typeof value === 'string' ? value : ''
}

function optionalFiniteNumber(value) {
  return typeof value === 'number' && Number.isFinite(value) ? value : null
}

function appendRecord(fd, record) {
  const line = `${JSON.stringify(record)}\n`
  if (Buffer.byteLength(line) > maximumRecordBytes) throw new Error(`JSONL record exceeds ${maximumRecordBytes} bytes`)
  appendFileSync(fd, line, { encoding: 'utf8' })
  fsyncSync(fd)
}

function waitOneSecond() {
  return new Promise(resolvePromise => setTimeout(resolvePromise, 1000))
}

async function run(options) {
  const outputFD = openSync(options.outputPath, 'a')
  const runContext = { workspace: process.cwd(), out: options.outputPath }
  const startedAt = Date.now()
  const deadline = startedAt + options.durationSeconds * 1000
  let stopCause = 'duration_elapsed'
  let knownBootID = ''
  let interrupted = false
  const signalHandler = () => { interrupted = true }
  process.on('SIGINT', signalHandler)
  process.on('SIGTERM', signalHandler)

  try {
    try {
      const identity = firmwareIdentity(curlCBOR(options.base, '/api/v2/info'))
      if (!identity.boot_id) throw new Error('info boot ID missing')
      knownBootID = identity.boot_id
      appendRecord(outputFD, { type: 'firmware_identity', timestamp: new Date().toISOString(), ...identity, ...runContext, jsonl_backend: 'cborg', http_client: 'curl.exe', source_address: sourceAddress, input: '/api/v2/info' })
    } catch (error) {
      appendRecord(outputFD, { type: 'firmware_identity_error', timestamp: new Date().toISOString(), ...runContext, input: '/api/v2/info', error: error.message })
    }

    let nextSampleAt = Date.now()
    while (Date.now() < deadline) {
      if (interrupted) { stopCause = 'signal'; break }
      if (existsSync(options.stopPath)) { stopCause = 'stop_file'; break }

      if (Date.now() >= nextSampleAt) {
        const timestamp = new Date().toISOString()
        const sample = { type: 'memory_sample', timestamp, ...runContext, input: ['/api/v2/summary', '/api/v2/tasks?offset=0&limit=8'], boot_id: knownBootID || null }
        let summaryBootID = ''
        let summaryOK = false
        try {
          const summary = memorySample(curlCBOR(options.base, '/api/v2/summary'))
          summaryBootID = summary.boot_id
          if (knownBootID && summary.boot_id !== knownBootID) {
            sample.boot_changed = true
            sample.previous_boot_id = knownBootID
          }
          sample.boot_id = summary.boot_id
          Object.assign(sample, summary)
          summaryOK = true
          knownBootID = summary.boot_id
        } catch (error) {
          sample.summary_error = error.message
        }
        const stopAfterSummary = existsSync(options.stopPath)
        if (stopAfterSummary) stopCause = 'stop_file'
        if (!stopAfterSummary && summaryOK) {
          try {
            const result = taskSample(curlCBOR(options.base, '/api/v2/tasks?offset=0&limit=8'))
            if (result.boot_id !== summaryBootID) {
              sample.boot_inconsistent = true
              sample.tasks_error = `tasks boot ID ${result.boot_id} does not match summary boot ID ${summaryBootID}`
            } else {
              sample.tasks_boot_id = result.boot_id
              sample.tasks = result.tasks
            }
          } catch (error) {
            sample.tasks_error = error.message
          }
        }
        appendRecord(outputFD, sample)
        console.log(`${timestamp} boot=${sample.boot_id ?? 'unknown'} heap_min=${sample.heap_min_free_bytes ?? 'error'} tasks=${sample.tasks?.length ?? 0} errors=${sample.summary_error || sample.tasks_error ? 1 : 0}`)
        if (stopAfterSummary) break
        nextSampleAt = Date.now() + options.intervalSeconds * 1000
      }

      await waitOneSecond()
    }
    if (Date.now() >= deadline) stopCause = 'duration_elapsed'
    appendRecord(outputFD, { type: 'sampler_stop', timestamp: new Date().toISOString(), ...runContext, elapsed_seconds: Math.round((Date.now() - startedAt) / 1000), stop_cause: stopCause, boot_id: knownBootID || null })
    console.log(`sampler stopped cause=${stopCause} output=${options.outputPath}`)
  } finally {
    process.off('SIGINT', signalHandler)
    process.off('SIGTERM', signalHandler)
    closeSync(outputFD)
  }
}

try {
  await run(parseArguments(process.argv.slice(2)))
} catch (error) {
  console.error(`sample-memory: ${error.message}`)
  process.exitCode = 2
}

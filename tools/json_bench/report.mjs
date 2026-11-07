#!/usr/bin/env node
import crypto from 'node:crypto';
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { execFileSync } from 'node:child_process';
import readline from 'node:readline';

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../..');
const buildRoot = path.join(root, 'local', 'backend26-idf');
const benchRoot = path.join(root, 'local');
const nightRoot = path.join(root, 'local', 'night26');
const fullImageIds = [...range(28, 35), ...range(40, 42), ...range(46, 51)];
const isolatedBenchIds = [...range(36, 39), ...range(43, 45)];
const generatedAt = argumentValue('--generated-at') ?? new Date().toISOString();

function range(first, last) {
	return Array.from({ length: last - first + 1 }, (_, index) => first + index);
}

function argumentValue(name) {
	const index = process.argv.indexOf(name);
	return index >= 0 ? process.argv[index + 1] : null;
}

function readJson(filePath) {
	try {
		return JSON.parse(fs.readFileSync(filePath, 'utf8'));
	} catch {
		return null;
	}
}

function sha256(filePath) {
	try {
		return crypto.createHash('sha256').update(fs.readFileSync(filePath)).digest('hex');
	} catch {
		return null;
	}
}

function configValues(filePath) {
	try {
		const values = new Map();
		for (const line of fs.readFileSync(filePath, 'utf8').split(/\r?\n/)) {
			let match = line.match(/^CONFIG_([A-Z0-9_]+)=(.*)$/);
			if (match) {
				values.set(match[1], match[2].replace(/^"|"$/g, ''));
				continue;
			}
			match = line.match(/^# CONFIG_([A-Z0-9_]+) is not set$/);
			if (match) values.set(match[1], 'n');
		}
		return values;
	} catch {
		return null;
	}
}

function numberValue(value) {
	if (value === undefined || value === null || value === '') return null;
	const result = Number(value);
	return Number.isFinite(result) ? result : null;
}

function boolValue(values, key) {
	if (!values?.has(key)) return null;
	return values.get(key) === 'y';
}

function selectedReader(values) {
	if (!values) return null;
	for (const [key, name] of [
		['NAWSERVER_JSON_BOUNDED', 'bounded'],
		['NAWSERVER_JSON_YYJSON', 'yyjson'],
		['NAWSERVER_JSON_RAPIDJSON', 'rapidjson'],
	]) {
		if (values.get(key) === 'y') return name;
	}
	return 'не выбран или не записан';
}

function staticDataBss(sizeData) {
	if (!Array.isArray(sizeData?.layout)) return null;
	let total = 0;
	let found = false;
	for (const region of sizeData.layout) {
		for (const name of ['.data', '.bss']) {
			const bytes = numberValue(region?.parts?.[name]?.size);
			if (bytes !== null) {
				total += bytes;
				found = true;
			}
		}
	}
	return found ? total : null;
}

function fileSize(filePath) {
	try {
		return fs.statSync(filePath).size;
	} catch {
		return null;
	}
}

function fullImageRecord(id) {
	const directory = path.join(buildRoot, String(id));
	const descriptionPath = path.join(directory, 'project_description.json');
	const description = readJson(descriptionPath);
	const sdkconfigPath = path.join(directory, 'sdkconfig');
	const values = configValues(sdkconfigPath);
	const sizePath = path.join(directory, 'size.json');
	const sizeData = readJson(sizePath);
	const binPath = description?.app_bin ? path.join(directory, description.app_bin) : null;
	const elfPath = description?.app_elf ? path.join(directory, description.app_elf) : null;
	const binBytes = binPath ? fileSize(binPath) : null;
	const elfBytes = elfPath ? fileSize(elfPath) : null;
	const missing = [];
	if (!description) missing.push('project_description.json');
	if (!values) missing.push('sdkconfig');
	if (!sizeData) missing.push('size.json');
	if (binBytes === null) missing.push(description?.app_bin ?? 'app.bin path');
	if (elfBytes === null) missing.push(description?.app_elf ?? 'app.elf path');
	return {
		id,
		kind: 'full-image',
		project: description?.project_name ?? null,
		board: description?.target ?? null,
		sdk: description?.idf_path ? path.basename(description.idf_path) : null,
		projectVersionLabel: description?.project_version ?? null,
		idfRevisionLabel: description?.git_revision ?? null,
		projectPath: description?.project_path ?? null,
		backend: boolValue(values, 'NAWSERVER_BACKEND_TCP') === true ? 'TCP'
			: boolValue(values, 'NAWSERVER_BACKEND_HTTPD') === true ? 'HTTPD' : null,
		reader: selectedReader(values),
		mcpEnabled: boolValue(values, 'NAWSERVER_MCP'),
		workspaceBytes: numberValue(values?.get('NAWSERVER_JSON_WORKSPACE_BYTES')),
		inputBytes: numberValue(values?.get('NAWSERVER_JSON_INPUT_BYTES')),
		outputBytes: numberValue(values?.get('NAWSERVER_MCP_OUTPUT_BYTES')),
		httpStackBytes: numberValue(values?.get('NAWSERVER_TCP_HTTP_STACK_SIZE'))
			?? numberValue(values?.get('NAWSERVER_HTTPD_STACK_SIZE'))
			?? numberValue(values?.get('HTTPD_TASK_STACK_SIZE')),
		binBytes,
		dataBssBytes: staticDataBss(sizeData),
		configSha256: sha256(sdkconfigPath),
		binSha256: binPath ? sha256(binPath) : null,
		elfSha256: elfPath ? sha256(elfPath) : null,
		artifactStatus: missing.length === 0 ? 'build artifacts present' : `unmeasured; missing: ${missing.join(', ')}`,
		runtimeStatus: 'not inferred from build artifacts',
	};
}

function consumerLabel(benchmark) {
	if (!benchmark) return null;
	const consumers = [];
	if (benchmark.existing_application_yyjson === true) consumers.push('yyjson');
	if (benchmark.existing_application_rapidjson === true) consumers.push('rapidjson');
	if (consumers.length) return consumers.join('+');
	if ('existing_application_yyjson' in benchmark || 'existing_application_rapidjson' in benchmark) return 'none';
	return null;
}

function isolatedBenchRecord(id) {
	const source = path.join(benchRoot, `json26-bench-${id}-result.json`);
	let sourceText = null;
	try { sourceText = fs.readFileSync(source, 'utf8'); } catch { /* missing result is unmeasured */ }
	const result = readJson(source);
	const benchmark = result?.benchmark ?? result;
	const build = fullImageRecord(id);
	const exactChecksum = sourceText?.match(/"checksum"\s*:\s*(\d+)/)?.[1] ?? null;
	return {
		id,
		kind: 'isolated-bench',
		board: result?.chip ?? build.board,
		sdk: build.sdk,
		projectVersionLabel: build.projectVersionLabel,
		reader: benchmark?.backend ?? build.reader,
		workspaceReservedBytes: numberValue(benchmark?.reserved_arena_bytes),
		workspaceObservedPeakBytes: numberValue(benchmark?.reader_parse_peak_bytes),
		elapsedUs: numberValue(benchmark?.elapsed_us),
		checksum: exactChecksum ?? (benchmark?.checksum === undefined ? null : String(benchmark.checksum)),
		applicationConsumer: consumerLabel(benchmark),
		benchmarkOk: typeof benchmark?.ok === 'boolean' ? benchmark.ok : null,
		configSha256: build.configSha256,
		binSha256: build.binSha256,
		resultStatus: result ? (benchmark?.ok === true ? 'recorded benchmark result: ok=true'
			: benchmark?.ok === false ? 'recorded benchmark result: ok=false' : 'result present; status unavailable') : 'unmeasured; result file missing',
	};
}

function parseDurationMs(text) {
	if (typeof text !== 'string') return null;
	const match = text.match(/^\s*(\d+(?:\.\d+)?)\s*(ms|s|m)\s*$/i);
	if (!match) return null;
	const value = Number(match[1]);
	return value * (match[2].toLowerCase() === 'ms' ? 1 : match[2].toLowerCase() === 's' ? 1000 : 60000);
}

function addMemoryValues(aggregates, event) {
	const regions = Array.isArray(event.memory) ? event.memory : null;
	const values = regions?.length ? regions.map((region, index) => ({
		id: String(region.id ?? region.label ?? `region-${index}`),
		freeBytes: numberValue(region.free_bytes),
	})) : [{
		id: 'summary.memory[0]',
		freeBytes: numberValue(event.heap_free_bytes),
	}];
	for (const region of values) {
		if (region.freeBytes === null) continue;
		const key = `${event.boot_id ?? 'unknown'}\u0000${region.id}`;
		let aggregate = aggregates.get(key);
		if (!aggregate) {
			aggregate = { bootId: event.boot_id ?? null, region: region.id, firstFreeBytes: region.freeBytes,
				lastFreeBytes: region.freeBytes, minFreeBytes: region.freeBytes, maxFreeBytes: region.freeBytes, samples: 0 };
			aggregates.set(key, aggregate);
		}
		aggregate.lastFreeBytes = region.freeBytes;
		aggregate.minFreeBytes = Math.min(aggregate.minFreeBytes, region.freeBytes);
		aggregate.maxFreeBytes = Math.max(aggregate.maxFreeBytes, region.freeBytes);
		aggregate.samples++;
	}
}

async function analyzeJsonl(filePath, profilesByElfHash) {
	const groups = new Map();
	const memoryRegions = new Map();
	const bootIds = new Set();
	let firstIdentity = null;
	let previousBootId = null;
	let bootChanges = 0;
	let samplerErrors = 0;
	let readErrors = 0;
	let memorySampleCount = 0;
	let samplerStop = null;
	let malformedLines = 0;
	let lineCount = 0;
	let stream;
	try {
		stream = fs.createReadStream(filePath, { encoding: 'utf8' });
	} catch {
		return { loadRecords: [], memoryRecord: null };
	}
	const lines = readline.createInterface({ input: stream, crlfDelay: Infinity });
	try {
	for await (const line of lines) {
		if (!line) continue;
		lineCount++;
		let event;
		try { event = JSON.parse(line); } catch { malformedLines++; continue; }
		if (!event || typeof event !== 'object') { malformedLines++; continue; }

		if (event.type === 'firmware_identity') {
			firstIdentity ??= event;
			if (event.boot_id) {
				bootIds.add(String(event.boot_id));
				previousBootId ??= event.boot_id;
			}
		}
		if (event.type === 'firmware_identity_error') samplerErrors++;
		if (event.type === 'sampler_stop') samplerStop = event;
		if (event.type === 'memory_sample') {
			memorySampleCount++;
			if (event.summary_error || event.tasks_error) samplerErrors++;
			if (event.boot_id) {
				bootIds.add(String(event.boot_id));
				const changedBoot = previousBootId && previousBootId !== event.boot_id;
				if (event.boot_changed === true || changedBoot) bootChanges++;
				previousBootId = event.boot_id;
			}
			addMemoryValues(memoryRegions, event);
		}

		if (event.type === 'stage' || event.type === 'sample') {
			const key = String(event.run_id ?? path.basename(filePath));
			let group = groups.get(key);
			if (!group) {
				group = { runId: key, start: null, final: null, latestSample: null, firstEvent: event };
				groups.set(key, group);
			}
			if (event.stage === 'mixed-load-start' && !group.start) group.start = event;
			if (event.stage === 'mixed-load-stop-and-recovery') group.final = event;
			if (event.type === 'sample' && event.counters) group.latestSample = event;
		}
	}
	} catch {
		readErrors++;
	}
	const fileName = path.basename(filePath);
	const loadRecords = [...groups.values()].map(group => {
		const start = group.start ?? group.firstEvent ?? {};
		const final = group.final;
		const latest = final ?? group.latestSample ?? group.firstEvent ?? {};
		const counters = latest.counters ?? {};
		const startDuration = parseDurationMs(start.duration);
		const durationMs = numberValue(final?.elapsed_ms) ?? numberValue(latest.elapsed_ms) ?? startDuration;
		const statuses = counters.http_status_counts ?? {};
		const observedAssetPaths = new Set();
		for (const key of Object.keys(statuses)) {
			const match = key.match(/^asset:(.*):\d{3}$/);
			if (match) observedAssetPaths.add(match[1]);
		}
		for (const key of Object.keys(counters.body_sha256 ?? {})) {
			const match = key.match(/^asset:(.*)$/);
			if (match) observedAssetPaths.add(match[1]);
		}
		const recovery = final?.recovery ?? null;
		const bootId = final?.boot_id ?? recovery?.boot_id ?? latest.boot_id ?? start.boot_id ?? null;
		const isSmoke = /smoke/i.test(fileName) || /smoke/i.test(String(start.stage ?? ''));
		const hasErrors = (numberValue(counters.errors) ?? 0) > 0 || (numberValue(counters.timeouts) ?? 0) > 0
			|| Object.keys(counters.body_integrity_failures ?? {}).length > 0;
		return {
			file: fileName,
			runId: group.runId,
			recordClass: isSmoke ? (hasErrors ? 'harness-smoke-with-errors' : 'harness-smoke')
				: final?.recovery ? 'final-load-record' : 'partial-or-unclassified',
			clients: numberValue(start.clients ?? latest.clients),
			durationMs,
			httpRequests: numberValue(counters.http_requests),
			mcpList: numberValue(counters.mcp_list),
			mcpEcho: numberValue(counters.mcp_echo),
			mcpInvalid: numberValue(counters.mcp_invalid),
			wsOpened: numberValue(counters.ws_opened),
			wsEchoSent: numberValue(counters.ws_echo_sent),
			wsEchoReceived: numberValue(counters.ws_echo_received),
			sseOpened: numberValue(counters.sse_opened),
			sseProgressEvents: numberValue(counters.sse_progress_events),
			errors: numberValue(counters.errors),
			timeouts: numberValue(counters.timeouts),
			httpStatusCounts: statuses,
			bootId,
			recovery,
			configuredAssetPaths: Array.isArray(start.asset_paths) ? start.asset_paths : [],
			observedAssetPaths: [...observedAssetPaths].sort(),
			malformedLines,
			finalStagePresent: Boolean(final),
			finalStageHasRecovery: Boolean(final?.recovery),
		};
	});
	let memoryRecord = null;
	if (memorySampleCount > 0 || firstIdentity || samplerErrors > 0) {
		const elfHash = typeof firstIdentity?.elf_sha256 === 'string' ? firstIdentity.elf_sha256.toLowerCase() : null;
		memoryRecord = {
			file: fileName,
			profile: elfHash ? profilesByElfHash.get(elfHash) ?? null : null,
			profileMatch: elfHash && profilesByElfHash.has(elfHash) ? 'matched by ELF SHA-256' : 'unmatched: no matching ELF SHA-256',
			firmwareIdentity: firstIdentity,
			sampleCount: memorySampleCount,
			bootIds: [...bootIds],
			bootChanges,
			errorCount: samplerErrors + malformedLines + readErrors,
			malformedLines,
			readErrors,
			regions: [...memoryRegions.values()].sort((left, right) => left.bootId?.localeCompare(right.bootId ?? '') || left.region.localeCompare(right.region)),
			stop: samplerStop,
		};
	}
	return { loadRecords, memoryRecord, lineCount };
}

function readRuntimeSnapshots(files, profiles) {
	const byProfile = new Map();
	const rejected = [];
	for (const filePath of files) {
		const file = path.basename(filePath);
		const snapshot = readJson(filePath);
		if (!snapshot) { rejected.push({ file, reason: 'invalid JSON' }); continue; }
		const id = numberValue(snapshot.profile);
		const profile = profiles.find(row => row.id === id);
		const info = snapshot.info;
		const summary = snapshot.summary;
		const tasks = snapshot.tasks;
		let reason = null;
		if (!profile) reason = 'profile ID not in report';
		else if (info?.source !== 'device') reason = 'info source is not device';
		else if (!profile.elfSha256) reason = 'profile ELF SHA-256 unavailable';
		else if (String(info?.details?.build?.elf_sha256 ?? '').toLowerCase() !== profile.elfSha256.toLowerCase()) reason = 'info build ELF SHA-256 does not match profile';
		else if (String(snapshot.elf_sha256 ?? '').toLowerCase() !== profile.elfSha256.toLowerCase()) reason = 'snapshot ELF SHA-256 does not match profile';
		else {
			const bootIds = [info?.boot_id, summary?.boot_id, tasks?.boot_id];
			if (bootIds.some(value => typeof value !== 'string' || value.length === 0) || new Set(bootIds).size !== 1) reason = 'info/summary/tasks boot IDs differ or missing';
			else if (summary?.source !== 'device' || tasks?.source !== 'device') reason = 'summary or tasks source is not device';
		}
		if (reason) { rejected.push({ file, profile: id, reason }); continue; }
		const measuredAt = typeof snapshot.measured_utc === 'string' ? snapshot.measured_utc : null;
		const measuredAtMs = measuredAt ? Date.parse(measuredAt) : Number.NaN;
		if (!Number.isFinite(measuredAtMs)) { rejected.push({ file, profile: id, reason: 'invalid measured_utc' }); continue; }
		const memory = Array.isArray(summary.data?.memory) ? summary.data.memory.map(region => ({
			id: String(region.id ?? region.label ?? 'unknown'),
			label: region.label ?? null,
			totalBytes: numberValue(region.total_bytes),
			freeBytes: numberValue(region.free_bytes),
			minFreeBytes: numberValue(region.min_free_bytes),
			largestBlockBytes: numberValue(region.largest_block_bytes),
		})) : [];
		const httpTask = Array.isArray(tasks.data?.items) ? tasks.data.items.find(task => task.name === 'naw_tcp_http') : null;
		const record = {
			file,
			profile: id,
			tag: snapshot.tag ?? null,
			measuredUtc: measuredAt,
			measuredAtMs,
			board: snapshot.board ?? null,
			reader: info.firmware?.json_backend ?? null,
			backend: info.firmware?.http_backend ?? null,
			bootId: info.boot_id,
			ipv4: info.details?.network?.ipv4 ?? null,
			elfSha256: profile.elfSha256,
			memory,
		httpTaskStackMinFreeBytes: numberValue(httpTask?.stack_min_free_bytes),
		};
		const prior = byProfile.get(id);
		if (!prior || record.measuredAtMs > prior.measuredAtMs) byProfile.set(id, record);
	}
	return { snapshots: [...byProfile.values()].sort((left, right) => left.profile - right.profile), rejected };
}

function gitEvidence() {
	try {
		const head = execFileSync('git', ['rev-parse', 'HEAD'], { cwd: root, encoding: 'utf8' }).trim();
		const dirty = execFileSync('git', ['status', '--porcelain'], { cwd: root, encoding: 'utf8' }).trim().length > 0;
		return { head, workingTreeDirty: dirty };
	} catch {
		return { head: null, workingTreeDirty: null };
	}
}

function escapeCell(value) {
	return String(value ?? '—').replaceAll('|', '\\|').replaceAll('\n', ' ');
}

function formatBytes(value) {
	return value === null ? '—' : String(value).replace(/\B(?=(\d{3})+(?!\d))/g, ',');
}

function formatHash(value) {
	return value ? value.slice(0, 12) : '—';
}

function writeAtomic(filePath, contents) {
	fs.mkdirSync(path.dirname(filePath), { recursive: true });
	const temporaryPath = path.join(path.dirname(filePath), `.${path.basename(filePath)}.${process.pid}.${crypto.randomUUID()}.tmp`);
	try {
		fs.writeFileSync(temporaryPath, contents, 'utf8');
		fs.renameSync(temporaryPath, filePath);
	} catch (error) {
		try { fs.unlinkSync(temporaryPath); } catch { /* temp may already be renamed or removed */ }
		throw error;
	}
}

function createMarkdown(matrix) {
	const lines = [
		'# Матрица сборок и измерений JSON',
		'',
		`Сформировано: ${matrix.generatedAt}. Источники — только сохранённые build/result JSON, sdkconfig, образы и JSONL под local/. Отсутствующие данные отмечены как «—»/«не измерено», значения не оцениваются.`,
		'',
		`Текущий checkout: HEAD \`${matrix.repository.head ?? 'не определён'}\`; рабочее дерево ${matrix.repository.workingTreeDirty === true ? 'изменено' : matrix.repository.workingTreeDirty === false ? 'чистое' : 'состояние неизвестно'}. Это состояние не доказывает, из какого точного исходного состояния собран каждый старый образ. В профиле показывается записанная метка project_version, config SHA и BIN SHA отдельно; если метки исходного проекта нет, source-stage не установлен.`,
		'',
		'Запись о наличии BIN/ELF/size.json означает только наличие build-артефактов; runtime-приёмка из неё не выводится. Статические `.data + .bss` — сумма полей `size.layout[].parts[".data"].size` и `.bss.size`; отсутствующий size report даёт неизвестное значение, не оценку.',
		'',
		'## Полные образы',
		'',
		'| ID | Проект / board | SDK | Backend / reader | MCP | Workspace / input / output | HTTP stack | BIN | data+bss | project_version | config SHA-256 | BIN SHA-256 | status |',
		'|---:|---|---|---|---:|---:|---:|---:|---:|---|---|---|---|',
	];
	for (const row of matrix.fullImages) {
		lines.push(`| ${row.id} | ${escapeCell(row.project)} / ${escapeCell(row.board)} | ${escapeCell(row.sdk)} | ${escapeCell(row.backend)} / ${escapeCell(row.reader)} | ${row.mcpEnabled === null ? '—' : row.mcpEnabled ? 'да' : 'нет'} | ${formatBytes(row.workspaceBytes)} / ${formatBytes(row.inputBytes)} / ${formatBytes(row.outputBytes)} | ${formatBytes(row.httpStackBytes)} | ${formatBytes(row.binBytes)} | ${formatBytes(row.dataBssBytes)} | ${escapeCell(row.projectVersionLabel ?? 'не записан')} | ${formatHash(row.configSha256)} | ${formatHash(row.binSha256)} | ${escapeCell(row.artifactStatus)}; ${escapeCell(row.runtimeStatus)} |`);
	}
	lines.push(
		'',
		'## Runtime snapshots, привязанные к ELF',
		'',
		'В таблицу попадают только device snapshots, у которых info.details.build.elf_sha256 совпадает с ELF профиля, внешний ELF hash совпадает и boot ID в info/summary/tasks одинаков. Это точечные наблюдения при внешней нагрузке, не контролируемый memory delta и не доказательство отсутствия/наличия утечки.',
		'',
		'| Профиль | Snapshot / tag | Reader / backend | Boot ID / IPv4 | RAM region | Total / free / min free / largest block | HTTP task stack min free |',
		'|---:|---|---|---|---|---:|---:|',
	);
	for (const snapshot of matrix.runtimeSnapshots) {
		for (const region of snapshot.memory) {
			lines.push(`| ${snapshot.profile} | ${escapeCell(snapshot.measuredUtc)} / ${escapeCell(snapshot.tag)} | ${escapeCell(snapshot.reader)} / ${escapeCell(snapshot.backend)} | ${escapeCell(snapshot.bootId)} / ${escapeCell(snapshot.ipv4)} | ${escapeCell(region.id)} | ${formatBytes(region.totalBytes)} / ${formatBytes(region.freeBytes)} / ${formatBytes(region.minFreeBytes)} / ${formatBytes(region.largestBlockBytes)} | ${formatBytes(snapshot.httpTaskStackMinFreeBytes)} |`);
		}
	}
	if (!matrix.runtimeSnapshots.length) lines.push('| — | Совпадающих snapshot файлов нет | — | — | — | — | — |');
	lines.push(
		'',
		'Сравнения свободной PSRAM между разными образами не трактуются как эффект JSON-backend: размещение rodata меняет общий пул, в том числе примерно на 64 KiB. Ошибочные/непривязанные snapshot перечислены в JSON matrix.rejectedSnapshots.',
		'',
		'## Изолированный JSON benchmark',
		'',
		'Ниже приведены только сохранённые результаты профилей 36–39 и 43–45. `ok` — поле самого benchmark JSON, а не общая приёмка прошивки.',
		'',
		'| ID | Board / SDK / reader | Arena reserved / observed peak | elapsed us | checksum | app consumer | result | config SHA-256 |',
		'|---:|---|---:|---:|---:|---|---|---|',
	);
	for (const row of matrix.isolatedBench) {
		lines.push(`| ${row.id} | ${escapeCell(row.board)} / ${escapeCell(row.sdk)} / ${escapeCell(row.reader)} | ${formatBytes(row.workspaceReservedBytes)} / ${formatBytes(row.workspaceObservedPeakBytes)} | ${formatBytes(row.elapsedUs)} | ${formatBytes(row.checksum)} | ${escapeCell(row.applicationConsumer ?? 'не записан')} | ${escapeCell(row.resultStatus)} | ${formatHash(row.configSha256)} |`);
	}
	lines.push(
		'',
		'## Сохранённые JSONL load records',
		'',
		'Это механическое извлечение счётчиков из журналов. Smoke/harness-прогоны показаны отдельно и не считаются результатом финальной прошивки; counters.errors отражает данные harness и сам по себе не локализует причину.',
		'SDK min_free_bytes суммирует исторические минимумы отдельных heap regions, достигнутые в разное время. Это не измерение одновременного остатка всей internal RAM; sampled free и largest block рассматриваются отдельно.',
		'Ограничение версии harness: ws_echo_sent и ws_echo_received увеличиваются вместе только после подтверждённого точного echo. Их равенство не доказывает доставку всех попыток; используйте число завершённых WS-обменов вместе с errors/timeouts. body_integrity_failures относится к HTTP-проверкам, а ошибки WS decoder/echo учитываются в общем errors. Ограниченная корзина error_reason_counts может объединять разные причины в other_capped.',
		'',
		'| Файл / run | Класс | Клиенты / длительность | HTTP | WS open / sent / received | SSE open / events | Errors / timeouts | Boot ID | Recovery | Активы из ответов |',
		'|---|---|---:|---:|---:|---:|---:|---|---|---|',
	);
	for (const row of matrix.loadRecords) {
		const recovery = row.recovery
			? `health=${row.recovery.health_status ?? '—'}, info=${row.recovery.info_status ?? '—'}`
			: row.finalStagePresent ? 'не записан' : 'нет финальной записи';
		const duration = row.durationMs === null ? '—' : `${formatBytes(row.durationMs)} ms`;
		lines.push(`| ${escapeCell(row.file)} / ${escapeCell(row.runId)} | ${escapeCell(row.recordClass)} | ${formatBytes(row.clients)} / ${duration} | ${formatBytes(row.httpRequests)} | ${formatBytes(row.wsOpened)} / ${formatBytes(row.wsEchoSent)} / ${formatBytes(row.wsEchoReceived)} | ${formatBytes(row.sseOpened)} / ${formatBytes(row.sseProgressEvents)} | ${formatBytes(row.errors)} / ${formatBytes(row.timeouts)} | ${escapeCell(row.bootId)} | ${escapeCell(recovery)} | configured: ${escapeCell(row.configuredAssetPaths.join(', ') || '—')}; observed: ${escapeCell(row.observedAssetPaths.join(', ') || '—')} |`);
	}
	if (!matrix.loadRecords.length) lines.push('| — | финальных JSONL записей нет | — | — | — | — | — | — | — | — |');
	lines.push(
		'',
		'## Memory sampler JSONL',
		'',
		'Агрегации строятся только из записей type=memory_sample; source/profile сопоставляется по ELF SHA-256, если sampler его записал. Свободная память показана механически по boot ID и region. Наличие короткого sampler JSONL само по себе не подтверждает ночной soak.',
		'',
		'| Файл | Profile match | Samples / boot changes / errors | Boot IDs | Region | First / min / max / last free |',
		'|---|---|---:|---|---|---:|',
	);
	for (const sampleFile of matrix.memorySamplerRecords) {
		if (!sampleFile.regions.length) {
			lines.push(`| ${escapeCell(sampleFile.file)} | ${escapeCell(sampleFile.profileMatch)} | ${formatBytes(sampleFile.sampleCount)} / ${formatBytes(sampleFile.bootChanges)} / ${formatBytes(sampleFile.errorCount)} | ${escapeCell(sampleFile.bootIds.join(', ') || '—')} | — | — |`);
			continue;
		}
		for (const region of sampleFile.regions) {
			lines.push(`| ${escapeCell(sampleFile.file)} | ${sampleFile.profile ?? '—'} (${escapeCell(sampleFile.profileMatch)}) | ${formatBytes(sampleFile.sampleCount)} / ${formatBytes(sampleFile.bootChanges)} / ${formatBytes(sampleFile.errorCount)} | ${escapeCell(sampleFile.bootIds.join(', ') || '—')} | ${escapeCell(region.region)} @ ${escapeCell(region.bootId)} (${formatBytes(region.samples)} samples) | ${formatBytes(region.firstFreeBytes)} / ${formatBytes(region.minFreeBytes)} / ${formatBytes(region.maxFreeBytes)} / ${formatBytes(region.lastFreeBytes)} |`);
		}
	}
	if (!matrix.memorySamplerRecords.length) lines.push('| — | memory sampler JSONL пока нет | — | — | — | — |');
	lines.push(
		'',
		`Финальных (не-smoke) записей с recovery: ${matrix.loadRecords.filter(row => row.recordClass === 'final-load-record').length}. Текущая рабочая копия может быть dirty; SHA конфигурации/образа фиксирует именно сохранённый файл, а не точную историю исходников.`,
		'',
	);
	return lines.join('\n');
}

async function main() {
	const currentHead = gitEvidence();
	const fullImages = fullImageIds.map(fullImageRecord);
	const isolatedBench = isolatedBenchIds.map(isolatedBenchRecord);
	let snapshotFiles = [];
	let jsonlFiles = [];
	try {
		const names = fs.readdirSync(nightRoot).sort();
		snapshotFiles = names.filter(name => /-(?:c6|s3)\.json$/i.test(name)).map(name => path.join(nightRoot, name));
		jsonlFiles = names.filter(name => name.endsWith('.jsonl')).map(name => path.join(nightRoot, name));
	} catch {
		// Missing local evidence is represented as unmeasured.
	}
	const profilesByElfHash = new Map(fullImages.filter(row => row.elfSha256)
		.map(row => [row.elfSha256.toLowerCase(), row.id]));
	const { snapshots: runtimeSnapshots, rejected: rejectedSnapshots } = readRuntimeSnapshots(snapshotFiles, fullImages);
	for (const profile of fullImages) {
		const snapshot = runtimeSnapshots.find(row => row.profile === profile.id);
		profile.runtimeStatus = snapshot
			? `matching point-in-time device snapshot at ${snapshot.measuredUtc}`
			: 'no matching device snapshot';
	}
	const loadRecords = [];
	const memorySamplerRecords = [];
	for (const filePath of jsonlFiles) {
		const analyzed = await analyzeJsonl(filePath, profilesByElfHash);
		loadRecords.push(...analyzed.loadRecords);
		if (analyzed.memoryRecord) memorySamplerRecords.push(analyzed.memoryRecord);
	}
	loadRecords.sort((left, right) => left.file.localeCompare(right.file) || left.runId.localeCompare(right.runId));
	memorySamplerRecords.sort((left, right) => left.file.localeCompare(right.file));

	const matrix = {
		schemaVersion: 1,
		generatedAt,
		repository: currentHead,
		sourceRevisionCaveat: 'Per-profile application source commit is reported only if present in the recorded project_version label; current checkout HEAD/dirty state does not prove artifact provenance.',
		fullImages,
		isolatedBench,
		runtimeSnapshots: runtimeSnapshots.map(({ measuredAtMs, ...snapshot }) => snapshot),
		rejectedSnapshots,
		loadRecords,
		memorySamplerRecords,
	};

	const jsonOutput = path.join(nightRoot, 'matrix.json');
	const markdownOutput = path.join(root, 'docs', 'readme_json_matrix_ru.md');
	writeAtomic(jsonOutput, `${JSON.stringify(matrix, null, 2)}\n`);
	writeAtomic(markdownOutput, createMarkdown(matrix));
	process.stdout.write(`Generated ${path.relative(root, markdownOutput)} and ${path.relative(root, jsonOutput)}\n`);
}

await main();

// Direct, bounded HTTP MCP client. This module has no Node or CBOR dependency.
export const MCP_VERSION = '2026-07-28';
export class McpClient {
  constructor(fetcher = globalThis.fetch.bind(globalThis)) {
    this.fetcher = fetcher; this.nextId = 0; this.ready = false; this.tools = []; this.protocol = ''; this.server = null; this.transcript = []; this.lastResponseText = '';
  }
  async rpc(method, params) {
    const id = ++this.nextId;
    const metadata = {'io.modelcontextprotocol/protocolVersion':MCP_VERSION,'io.modelcontextprotocol/clientCapabilities':{},'io.modelcontextprotocol/clientInfo':{name:'NaWServer browser laboratory',version:'1.0'}};
    const request = {jsonrpc:'2.0', id, method, params:{...(params??{}),_meta:metadata}};
    const body = JSON.stringify(request);
    if (new TextEncoder().encode(body).length > 4096) throw Error('Request exceeds 4096 bytes');
    const headers = {'Content-Type':'application/json', Accept:'application/json, text/event-stream', 'MCP-Protocol-Version':MCP_VERSION, 'Mcp-Method':method};
    if (method === 'tools/call') headers['Mcp-Name'] = params.name;
    const exchange = {method, request, status:0, response:''};
    this.transcript.unshift(exchange); this.transcript.length = Math.min(this.transcript.length, 12);
    const response = await this.fetcher('/mcp', {method:'POST', headers, body, credentials:'omit', cache:'no-store', signal:AbortSignal.timeout(5000)});
    exchange.status = response.status;
    // Read incrementally so an invalid peer cannot allocate an unbounded response.
    const reader = response.body?.getReader(); const chunks = []; let length = 0;
    try {
      if (reader) for (;;) {
        const item = await reader.read(); if (item.done) break;
        length += item.value.length;
        if (length > 16384) { await reader.cancel(); throw Error('Response exceeds 16384 bytes'); }
        chunks.push(item.value);
      }
    } finally { reader?.releaseLock(); }
    const bytes = new Uint8Array(length); let offset = 0;
    for (const chunk of chunks) { bytes.set(chunk,offset); offset += chunk.length; }
    const text = new TextDecoder('utf-8',{fatal:true}).decode(bytes); exchange.response = text; this.lastResponseText = text;
    if (!response.ok) {
      let rpcError;
      try { const message=JSON.parse(text); if(message.jsonrpc==='2.0' && message.id===id) rpcError=message.error; } catch {}
      if(rpcError) throw Error(`HTTP ${response.status} · JSON-RPC ${rpcError.code} · ${rpcError.message}`);
      throw Error(response.status === 404 ? 'HTTP 404 · MCP endpoint unavailable / disabled' : `HTTP ${response.status} · ${text.slice(0,256)}`);
    }

    if (!response.headers.get('content-type')?.startsWith('application/json')) throw Error('Expected application/json (embedded stateless profile)');
    const message = JSON.parse(text);
    if (message.jsonrpc !== '2.0' || message.id !== id) throw Error('Invalid JSON-RPC response / id');
    if (message.error) throw Error(`JSON-RPC ${message.error.code} · ${message.error.message}`);
    if (!Object.hasOwn(message,'result')) throw Error('JSON-RPC result missing');
    if (message.result?.resultType !== 'complete') throw Error('Unsupported MCP result type');
    return message.result;
  }
  async discover() {
    this.ready = false; this.tools = []; this.protocol = ''; this.server = null;
    const result = await this.rpc('server/discover',{});
    if (!Array.isArray(result.supportedVersions) || !result.supportedVersions.includes(MCP_VERSION)) throw Error('Server does not support MCP '+MCP_VERSION);
    this.protocol = MCP_VERSION; this.server = result;
    if (!result.capabilities?.tools) throw Error('Server does not advertise tools capability');
    const listed = await this.rpc('tools/list',{});
    if (!Array.isArray(listed.tools) || listed.nextCursor || listed.tools.length > 32) throw Error('Unsupported / oversized tools list');
    if (listed.tools.some(tool => !tool || typeof tool.name !== 'string' || !tool.name || !tool.inputSchema || typeof tool.inputSchema !== 'object' || Array.isArray(tool.inputSchema))) throw Error('Invalid tool definition');
    this.tools = listed.tools; this.ready = true; return result;
  }
  async call(name, argumentText) {
    if (!this.ready || !this.tools.some(tool => tool.name === name)) throw Error('Discover and select an advertised tool');
    const args = JSON.parse(argumentText);
    if (!args || Array.isArray(args) || typeof args !== 'object') throw Error('Tool arguments must be a JSON object');
    // JS numbers cannot represent the full integer range accepted by the MCU.
    // Reject before serialization rather than silently changing an argument.
    const pending = [args];
    while(pending.length) {
      const value = pending.pop();
      if(typeof value === 'number' && (!Number.isFinite(value) || (Number.isInteger(value) && !Number.isSafeInteger(value)))) throw Error('Browser tester integer range is limited to ±9007199254740991; argument was not sent');
      if(value && typeof value === 'object') pending.push(...Object.values(value));
    }
    return this.rpc('tools/call',{name,arguments:args});
  }
}

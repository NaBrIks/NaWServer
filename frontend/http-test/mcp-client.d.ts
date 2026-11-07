export const MCP_VERSION: string;
export interface McpTool {name:string;description?:string;inputSchema:Record<string,unknown>;annotations?:{readOnlyHint?:boolean}}
export class McpClient {
 constructor(fetcher?:typeof fetch);
 lastResponseText:string;ready:boolean;tools:McpTool[];protocol:string;server:Record<string,any>|null;
 transcript:{method:string;request:unknown;status:number;response:string}[];
 discover():Promise<Record<string,any>>;
 call(name:string,argumentText:string):Promise<any>;
 rpc(method:string,params?:unknown):Promise<any>;
}

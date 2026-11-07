using System.Text;
using System.Text.Json;
using Showcase.Model;
namespace Showcase.Host;

// Application simulator implementation, independent of the MCU C++ JSON backend.
// Tools reuse application model operations. The HTTP route enforces the same Basic policy as command APIs.
public sealed class SimulatorMcp(DeviceModel model)
{
    public const string Version="2026-07-28";
    static readonly JsonSerializerOptions ModelJson=new(){PropertyNamingPolicy=JsonNamingPolicy.SnakeCaseLower};
    static readonly HashSet<string> ExperimentFields=new(StringComparer.Ordinal){"action","kind","duration_ms","on_ms","period_ms","core","priority"};
    public sealed record Reply(int Status,byte[] Body);
    static Reply Encode(object value,int status=200){var bytes=JsonSerializer.SerializeToUtf8Bytes(value);return bytes.Length<=4096?new(status,bytes):new(500,[]);}
    static Reply Error(object? id,int code,string message,int status=200)=>Encode(new{jsonrpc="2.0",id,error=new{code,message}},status);
    static bool Object(JsonElement value)=>value.ValueKind==JsonValueKind.Object;
    static JsonElement Member(JsonElement value,string name)=>Object(value)&&value.TryGetProperty(name,out var member)?member:default;
    static string? Text(JsonElement value)=>value.ValueKind==JsonValueKind.String?value.GetString():null;
    static bool Validate(JsonElement value,ref int tokens){
        if(++tokens>128)return false;
        if(value.ValueKind==JsonValueKind.String)return Encoding.UTF8.GetByteCount(value.GetString()!)<=512;
        if(Object(value)){var names=new HashSet<string>(StringComparer.Ordinal);foreach(var property in value.EnumerateObject()){if(++tokens>128||Encoding.UTF8.GetByteCount(property.Name)>512||!names.Add(property.Name)||!Validate(property.Value,ref tokens))return false;}}
        if(value.ValueKind==JsonValueKind.Array)foreach(var item in value.EnumerateArray())if(!Validate(item,ref tokens))return false;
        if(value.ValueKind==JsonValueKind.Number){var raw=value.GetRawText();if(!raw.Contains('.')&&!raw.Contains('e')&&!raw.Contains('E'))return value.TryGetInt64(out _)||value.TryGetUInt64(out _);return value.TryGetDouble(out var number)&&double.IsFinite(number);}
        return true;
    }
    static bool Integer(JsonElement arguments,string name,int fallback,out int value){
        var field=Member(arguments,name);value=fallback;
        return field.ValueKind==JsonValueKind.Undefined||(field.ValueKind==JsonValueKind.Number&&field.TryGetInt32(out value));
    }
    public Reply Dispatch(ReadOnlyMemory<byte> bytes,string version,string methodHeader,string nameHeader,bool invalidHeaders=false){
        if(bytes.Length>4096)return new(413,[]);
        JsonDocument document;
        try{document=JsonDocument.Parse(bytes,new(){MaxDepth=8});}catch(JsonException){return Error(null,-32700,"Parse error");}
        using(document){
            var root=document.RootElement;int tokens=0;try{if(!Validate(root,ref tokens))return Error(null,-32700,"JSON resource limit / duplicate key");}catch(InvalidOperationException){return Error(null,-32700,"Invalid Unicode");}
            var id=Member(root,"id");object? responseId=id.ValueKind==JsonValueKind.Undefined?null:id;
            if(id.ValueKind!=JsonValueKind.Undefined&&id.ValueKind!=JsonValueKind.String&&!(id.ValueKind==JsonValueKind.Number&&(id.TryGetInt64(out _)||id.TryGetUInt64(out _))))return Error(null,-32600,"Invalid request id");
            var method=Text(Member(root,"method"));var parameters=Member(root,"params");
            if(!Object(root)||Text(Member(root,"jsonrpc"))!="2.0"||method is null||!Object(parameters))return Error(responseId,-32600,"Invalid Request");
            var metadata=Member(parameters,"_meta");var bodyVersion=Text(Member(metadata,"io.modelcontextprotocol/protocolVersion"));
            if(invalidHeaders)return Error(responseId,-32020,"Invalid HTTP protocol metadata",400);
            if(bodyVersion!=null&&version.Length>0&&version!=bodyVersion)return Error(responseId,-32020,"HTTP/body protocol version mismatch",400);
            if((version.Length>0&&version!=Version)||(bodyVersion!=null&&bodyVersion!=Version))return Encode(new{jsonrpc="2.0",id=responseId,error=new{code=-32022,message="Unsupported protocol version",data=new{supported=new[]{Version},requested=bodyVersion??version}}},400);
            if(bodyVersion is null||!Object(Member(metadata,"io.modelcontextprotocol/clientCapabilities")))return Error(responseId,-32602,"Required request metadata missing",400);
            if(version!=Version||methodHeader!=method)return Error(responseId,-32020,"HTTP protocol/method metadata mismatch",400);
            var name=Text(Member(parameters,"name"));
            if(method=="tools/call"&&(name is null||name!=nameHeader))return Error(responseId,-32020,"HTTP tool name metadata mismatch",400);
            if(id.ValueKind==JsonValueKind.Undefined)return new(202,[]);
            object result;
            if(method=="server/discover")result=new{resultType="complete",supportedVersions=new[]{Version},capabilities=new{tools=new{}},_meta=new Dictionary<string,object>{{"io.modelcontextprotocol/serverInfo",new{name="NaWServer Showcase simulator",version="0.1.0"}}}};
            else if(method=="ping")result=new{resultType="complete"};
            else if(method=="tools/list"){
                if(Member(parameters,"cursor").ValueKind!=JsonValueKind.Undefined)return Error(responseId,-32602,"Pagination unsupported");
                result=new{resultType="complete",tools=new object[]{new{name="device_info",description="Read simulator model identity; no hardware attached",inputSchema=new{type="object",properties=new{},additionalProperties=false},annotations=new{readOnlyHint=true}},new{name="echo",description="Echo at most 256 decoded UTF-8 bytes",inputSchema=new{type="object",properties=new{text=new{type="string",maxLength=256}},required=new[]{"text"},additionalProperties=false},annotations=new{readOnlyHint=true}},
                    new{name="experiment_status",description="Read the current application experiment state",inputSchema=new{type="object",properties=new{},additionalProperties=false},annotations=new{readOnlyHint=true}},
                    new{name="experiment_set",description="Start or stop the existing bounded demo experiment; uses application validation",inputSchema=new{type="object",properties=new{
                        action=new{type="string",@enum=new[]{"start","stop"}},kind=new{type="string",@enum=new[]{"pi","pulse"},@default="pi"},
                        duration_ms=new{type="integer",minimum=1000,maximum=30000,@default=5000},on_ms=new{type="integer",minimum=1,maximum=5000,@default=200},period_ms=new{type="integer",minimum=20,maximum=5000,@default=1000},core=new{type="integer",minimum=0,maximum=1,@default=0},priority=new{type="integer",minimum=1,maximum=5,@default=2}
                    },required=new[]{"action"},additionalProperties=false},annotations=new{readOnlyHint=false,destructiveHint=false,idempotentHint=false}}}};
            }else if(method=="tools/call"){
                var arguments=Member(parameters,"arguments");if(!Object(arguments))return Error(responseId,-32602,"Arguments must be an object");
                string text;
                if(name=="device_info"){
                    if(arguments.EnumerateObject().Any())return Error(responseId,-32602,"Invalid tool arguments");
                    var state=model.State();text=JsonSerializer.Serialize(new{source="simulator",target="showcase-model",sdk=".NET",uptime_ms=state.TimeMs,boot_id=state.BootId,profile=state.Profile,read_only=true});
                }else if(name=="echo"){
                    var value=Text(Member(arguments,"text"));if(value is null||arguments.EnumerateObject().Count()!=1||Encoding.UTF8.GetByteCount(value)>256)return Error(responseId,-32602,"Invalid tool arguments");text=value;
                }else if(name=="experiment_status"){
                    if(arguments.EnumerateObject().Any())return Error(responseId,-32602,"Invalid tool arguments");
                    text=JsonSerializer.Serialize(model.Experiment(),ModelJson);
                }else if(name=="experiment_set"){
                    var action=Text(Member(arguments,"action"));var kindValue=Member(arguments,"kind");
                    if(action is null||arguments.EnumerateObject().Any(property=>!ExperimentFields.Contains(property.Name))
                        ||(kindValue.ValueKind!=JsonValueKind.Undefined&&kindValue.ValueKind!=JsonValueKind.String)
                        ||!Integer(arguments,"duration_ms",5000,out var duration)||!Integer(arguments,"on_ms",200,out var on)
                        ||!Integer(arguments,"period_ms",1000,out var period)||!Integer(arguments,"core",0,out var core)
                        ||!Integer(arguments,"priority",2,out var priority))return Error(responseId,-32602,"Invalid tool arguments");
                    // Domain bounds and running-state checks remain in the same application method used by REST.
                    var error=model.SetExperiment(action,Text(kindValue)??"pi",duration,on,period,core,priority);
                    if(error is not null)return Error(responseId,-32602,error);
                    text=JsonSerializer.Serialize(model.Experiment(),ModelJson);
                }else return Error(responseId,-32602,"Unknown tool");
                result=new{resultType="complete",content=new[]{new{type="text",text}},isError=false};
            }else return Error(responseId,-32601,"Method not found",404);
            return Encode(new{jsonrpc="2.0",id=responseId,result});
        }
    }
}

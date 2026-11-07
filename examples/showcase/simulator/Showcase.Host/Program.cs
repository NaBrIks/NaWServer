using System.Net;
using System.Text;
using System.Formats.Cbor;
using Showcase.Contracts;
using Showcase.Host;
using Showcase.Model;
var builder=WebApplication.CreateBuilder(args);
var listenUrls=builder.Configuration["urls"]??"http://127.0.0.1:8788";
foreach(var address in listenUrls.Split(';'))if(!Uri.TryCreate(address,UriKind.Absolute,out var uri)||uri.Scheme!="http"||uri.Port==443||!IPAddress.TryParse(uri.Host,out var listenIp)||!IPAddress.IsLoopback(listenIp))throw new InvalidOperationException("Simulator requires explicit HTTP loopback URLs with a port other than 443");
builder.WebHost.UseUrls(listenUrls);
builder.WebHost.ConfigureKestrel(o=>{o.Limits.MaxRequestBodySize=4096;o.Limits.RequestHeadersTimeout=TimeSpan.FromSeconds(5);});
builder.Services.AddSingleton<DeviceModel>();builder.Services.AddSingleton<Traffic>();builder.Services.AddSingleton<SocketHub>();builder.Services.AddHostedService<SimulationWorker>();
var app=builder.Build();var model=app.Services.GetRequiredService<DeviceModel>();var traffic=app.Services.GetRequiredService<Traffic>();var sockets=app.Services.GetRequiredService<SocketHub>();
var diagnosticEvents=new DiagnosticEvents();
app.Use(async(c,next)=>{
    if(c.Connection.RemoteIpAddress is not {} ip||!IPAddress.IsLoopback(ip)||c.Request.Host.Host is not ("127.0.0.1" or "localhost" or "::1")){await CborHttp.Error("forbidden",403).ExecuteAsync(c);return;}
    var origin=c.Request.Headers.Origin.ToString();if(c.Request.Path!="/mcp"&&origin.Length>0&&origin!=$"{c.Request.Scheme}://{c.Request.Host}"){await CborHttp.Error("origin_rejected",403).ExecuteAsync(c);return;}
    c.Response.Headers["X-Content-Type-Options"]="nosniff";c.Response.Headers["X-Showcase-Source"]="simulator";
    c.Response.Headers["Content-Security-Policy"]="default-src 'self'; img-src 'self' data:; style-src 'self' 'unsafe-inline'; script-src 'self'; connect-src 'self'; object-src 'none'; base-uri 'none'; frame-ancestors 'none'";
    c.Response.Headers.CacheControl="no-store";Interlocked.Increment(ref traffic.Http);
    if(model.NetworkOffline&&(c.Request.Path.StartsWithSegments("/api")||c.Request.Path=="/ws")){await CborHttp.Error("simulated_offline",503).ExecuteAsync(c);return;}
    try{await next();}catch(Exception e)when(e is CborContentException or BadHttpRequestException){if(c.Response.HasStarted)throw;await CborHttp.Error("invalid_payload",e is BadHttpRequestException b?b.StatusCode:400).ExecuteAsync(c);}
});
var mcp=new SimulatorMcp(model);
app.Map("/mcp",async(HttpContext c)=>{if(!Auth(c)){await Unauthorized(c).ExecuteAsync(c);return;}await SimulatorMcpHttp.Handle(c,mcp);});
app.UseWebSockets(new WebSocketOptions{KeepAliveInterval=TimeSpan.FromSeconds(15)});
app.MapGet("/health",()=>CborHttp.Reply(new{ok=true,source="simulator",v=2}));
app.MapGet("/api/v2/info",()=>CborHttp.Reply(model.Info()));
app.MapGet("/api/v2/summary",()=>CborHttp.Reply(model.Summary(traffic.Snapshot(),sockets.Count)));
app.MapGet("/api/v2/tasks",(int? offset,int? limit,string? snapshot_id)=>{int start=offset??0,count=limit??8;if(start<0||start>64||count<1||count>8)return CborHttp.Error("invalid_argument",400);try{return CborHttp.Reply(model.Tasks(start,count,snapshot_id));}catch(InvalidOperationException){return CborHttp.Error("snapshot_changed",409);}});
bool Auth(HttpContext c){try{var h=c.Request.Headers.Authorization.ToString();return h.StartsWith("Basic ",StringComparison.Ordinal)&&Encoding.UTF8.GetString(Convert.FromBase64String(h[6..]))=="demo:showcase-only";}catch(FormatException){return false;}}
IResult Unauthorized(HttpContext c){c.Response.Headers.WWWAuthenticate="Basic realm=\"showcase\"";return CborHttp.Error("unauthorized",401);}
app.MapGet("/api/v2/auth/status",(HttpContext c)=>Auth(c)?Results.Text("Authenticated: demo; permission mask: 1"):Unauthorized(c));
app.MapGet("/api/v2/auth/admin",(HttpContext c)=>Auth(c)?CborHttp.Error("forbidden",403):Unauthorized(c));
app.MapGet("/api/v2/events",(HttpContext c)=>diagnosticEvents.Handle(c,model,traffic,sockets));
app.MapGet("/api/v2/events/private",async(HttpContext c)=>{
    if(!Auth(c)){await Unauthorized(c).ExecuteAsync(c);return;}
    await diagnosticEvents.Handle(c,model,traffic,sockets);
});
app.MapGet("/api/v2/connections",(HttpContext c,int? offset,int? limit)=>{if(!Auth(c))return Unauthorized(c);int start=offset??0,count=limit??8;if(start<0||count<1||count>8)return CborHttp.Error("invalid_argument",400);var all=sockets.Connections();return CborHttp.Reply(new ConnectionPage(start,start+count<all.Length?start+count:null,all.Length,all.Skip(start).Take(count).ToArray()));});
// Text/form are explicit HTTP teaching routes, not telemetry or command encoding.
app.MapMethods("/api/v2/echo",["GET","POST"],async(HttpContext c)=>{if(c.Request.Method=="GET")return Results.Text(c.Request.Query["value"].ToString(),"text/plain; charset=utf-8");if(!c.Request.HasFormContentType)return CborHttp.Error("unsupported_media_type",415);var form=await c.Request.ReadFormAsync();return Results.Text(form["value"].ToString(),"text/plain; charset=utf-8");});
app.MapGet("/api/v2/stream",()=>Results.Text("headers → buffered content → response. Not SSE.","text/plain; charset=utf-8"));
bool Version(Dictionary<string,object?> body)=>body.GetValueOrDefault("v") is ulong v&&v==2;
var controlProfile="balanced";
app.MapPost("/api/v2/control",async(HttpContext c)=>{if(!Auth(c))return Unauthorized(c);if(!CborHttp.IsCbor(c.Request))return CborHttp.Error("unsupported_media_type",415);var body=await CborHttp.Body(c);if(!Version(body)||body.GetValueOrDefault("profile") is not string profile||profile is not ("balanced" or "quiet" or "performance"))return CborHttp.Error("invalid_argument",400);controlProfile=profile;return CborHttp.Reply(new{v=2,profile=controlProfile,storage="RAM only",effect="demo only"});});
int Field(Dictionary<string,object?> body,string key,int fallback){var raw=body.GetValueOrDefault(key);if(raw is null)return fallback;if(raw is ulong u&&u<=int.MaxValue)return (int)u;if(raw is long l&&l>=int.MinValue&&l<=int.MaxValue)return (int)l;throw new CborContentException("Integer field required");}
app.MapGet("/api/v2/experiments",()=>CborHttp.Reply(model.Experiment()));
app.MapPost("/api/v2/experiments",async(HttpContext c)=>{if(!Auth(c))return Unauthorized(c);if(!CborHttp.IsCbor(c.Request))return CborHttp.Error("unsupported_media_type",415);var b=await CborHttp.Body(c);if(!Version(b))return CborHttp.Error("invalid_argument",400);var error=model.SetExperiment(b.GetValueOrDefault("action") as string??"",b.GetValueOrDefault("kind") as string??"pi",Field(b,"duration_ms",5000),Field(b,"on_ms",200),Field(b,"period_ms",1000),Field(b,"core",0),Field(b,"priority",2));return error is null?CborHttp.Reply(model.Experiment()):CborHttp.Error("invalid_argument",400,error);});
app.MapGet("/api/v2/led",()=>CborHttp.Reply(model.Led()));
app.MapPost("/api/v2/led",async(HttpContext c)=>{if(!Auth(c))return Unauthorized(c);if(!CborHttp.IsCbor(c.Request))return CborHttp.Error("unsupported_media_type",415);var b=await CborHttp.Body(c);if(!Version(b)||b.GetValueOrDefault("enabled") is not bool enabled)return CborHttp.Error("invalid_argument",400);var error=model.SetLed(enabled,Field(b,"period_ms",1000),Field(b,"duty_pct",50));return error is null?CborHttp.Reply(model.Led()):CborHttp.Error("invalid_argument",400,error);});
app.MapGet("/api/v2/strip",()=>CborHttp.Reply(model.Strip()));
app.MapPost("/api/v2/strip",async(HttpContext c)=>{if(!Auth(c))return Unauthorized(c);if(!CborHttp.IsCbor(c.Request))return CborHttp.Error("unsupported_media_type",415);var b=await CborHttp.Body(c);if(!Version(b)||b.GetValueOrDefault("enabled") is not bool enabled||b.GetValueOrDefault("mode") is not string mode)return CborHttp.Error("invalid_argument",400);if(b.ContainsKey("driver")&&b["driver"] is not string)return CborHttp.Error("invalid_argument",400);var error=model.SetStrip(enabled,mode,Field(b,"color",0xffa030),Field(b,"brightness_pct",20),Field(b,"period_ms",1000),b.GetValueOrDefault("driver") as string);return error is null?CborHttp.Reply(model.Strip()):CborHttp.Error("invalid_argument",400,error);});
app.MapGet("/sim/api/v2/state",()=>CborHttp.Reply(model.State()));app.MapGet("/sim/api/v2/events",(long? after)=>CborHttp.Reply(model.Events(after??0)));
app.MapPost("/sim/api/v2/command",async(HttpContext c)=>{if(!CborHttp.IsCbor(c.Request))return CborHttp.Error("unsupported_media_type",415);var body=await CborHttp.Body(c);if(!Version(body)||body.GetValueOrDefault("action") is not string action)return CborHttp.Error("invalid_argument",400);
    var raw=body.GetValueOrDefault("number");if(raw is not null&&raw is not (ulong or long or double))return CborHttp.Error("invalid_argument",400);double? number=raw is null?null:Convert.ToDouble(raw);
    if(body.GetValueOrDefault("value") is not (null or string))return CborHttp.Error("invalid_argument",400);
    if(action=="disconnect"){sockets.Disconnect();return CborHttp.Reply(model.State());}
    string? error=model.Apply(new(action,body.GetValueOrDefault("value") as string,number));return error is null?CborHttp.Reply(model.State()):CborHttp.Error("invalid_argument",400,error);
});
app.Map("/ws",(HttpContext c)=>sockets.Handle(c,model));app.MapGet("/go",()=>Results.Redirect("/#capabilities"));
var assets=Path.GetFullPath(Path.Combine(app.Environment.ContentRootPath,"../../frontend/dist"));
SimulatorAssets.Map(app,assets);
app.MapFallback(()=>CborHttp.Error("not_found",404));
app.Logger.LogInformation("SYNTHETIC ESP32 simulator. Loopback only. Assets: {Assets}. No controller connected.",assets);
app.Run();

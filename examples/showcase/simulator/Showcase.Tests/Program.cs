using System.Net;
using System.Net.Http.Headers;
using System.Net.WebSockets;
using System.Formats.Cbor;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using Showcase.Contracts;
using Showcase.Model;

int passed=0;
void Check(bool condition,string label){if(!condition)throw new Exception(label);Console.WriteLine("PASS "+label);passed++;}
var mcpModel=new DeviceModel();
Check(mcpModel.Info().Firmware.JsonBackend=="system_text_json"&&mcpModel.Info().Firmware.McpEnabled,"INFO reports actual simulator MCP JSON backend");
var mcp=new Showcase.Host.SimulatorMcp(mcpModel);
byte[] McpRequest(string method,object? arguments=null,string name="echo",object? id=null)=>JsonSerializer.SerializeToUtf8Bytes(new{jsonrpc="2.0",id=id??1,method,@params=new Dictionary<string,object?>{{"_meta",new Dictionary<string,object>{{"io.modelcontextprotocol/protocolVersion",Showcase.Host.SimulatorMcp.Version},{"io.modelcontextprotocol/clientCapabilities",new{}}}},{"name",name},{"arguments",arguments??new{}}}});
Showcase.Host.SimulatorMcp.Reply McpCall(string method,object? arguments=null,string name="echo")=>mcp.Dispatch(McpRequest(method,arguments,name),Showcase.Host.SimulatorMcp.Version,method,name);
string McpText(Showcase.Host.SimulatorMcp.Reply reply)=>Encoding.UTF8.GetString(reply.Body);
Check(McpText(McpCall("server/discover")).Contains("supportedVersions"),"MCP modern discovery");
Check(McpText(McpCall("tools/list")).Contains("device_info"),"MCP static read-only tools");
var modelBefore=JsonSerializer.Serialize(mcpModel.State());
Check(McpText(McpCall("tools/call",new{},"device_info")).Contains("simulator"),"MCP device_info identifies simulator");
Check(McpText(McpCall("tools/call",new{text="hello"})).Contains("hello"),"MCP echo");
Check(JsonSerializer.Serialize(mcpModel.State())==modelBefore,"MCP calls do not mutate model");
Check(McpText(McpCall("tools/call",new{text=new string('я',129)})).Contains("-32602"),"MCP decoded UTF-8 byte limit");
Check(McpText(McpCall("tools/call",new{text="x",extra=1})).Contains("-32602"),"MCP extra tool arguments rejected");
Check(mcp.Dispatch(new byte[4097],"","","").Status==413,"MCP input bound");
Check(McpText(mcp.Dispatch(Encoding.UTF8.GetBytes("{\"jsonrpc\":\"2.0\",\"jsonrpc\":\"2.0\"}"),"","","")).Contains("-32700"),"MCP duplicate keys rejected");
Check(McpText(mcp.Dispatch(Encoding.UTF8.GetBytes("["+new string('[',8)+new string(']',8)+"]"),"","","")).Contains("-32700"),"MCP depth bound");
Check(mcp.Dispatch(McpRequest("server/discover"),Showcase.Host.SimulatorMcp.Version,"wrong","").Status==400,"MCP HTTP method mirror");
var integerId=McpText(mcp.Dispatch(McpRequest("ping",id:ulong.MaxValue),Showcase.Host.SimulatorMcp.Version,"ping",""));
Check(integerId.Contains("18446744073709551615"),"MCP uint64 id exact round trip");
Check(McpText(mcp.Dispatch(Encoding.UTF8.GetBytes("{\"x\":\"\\uD800\"}"),"","","")).Contains("-32700"),"MCP unpaired surrogate rejected");
Check(McpText(mcp.Dispatch(new byte[]{123,34,120,34,58,34,255,34,125},"","","")).Contains("-32700"),"MCP invalid UTF-8 rejected");
Check(McpText(mcp.Dispatch(McpRequest("ping"),Showcase.Host.SimulatorMcp.Version,"ping","",true)).Contains("-32020"),"MCP duplicate HTTP metadata rejected");
var infoWire=Map(CborCodec.Decode(CborCodec.Encode(mcpModel.Info())));
var firmwareWire=Map(infoWire["firmware"]);
Check(firmwareWire["json_backend"] as string=="system_text_json"&&firmwareWire["mcp_enabled"] is true,"INFO CBOR uses named MCP backend string and boolean");
using(var toolsDocument=JsonDocument.Parse(McpCall("tools/list").Body)){
 var tools=toolsDocument.RootElement.GetProperty("result").GetProperty("tools").EnumerateArray().ToArray();
 Check(tools.Any(tool=>tool.GetProperty("name").GetString()=="experiment_status"&&tool.GetProperty("annotations").GetProperty("readOnlyHint").GetBoolean())
    &&tools.Any(tool=>tool.GetProperty("name").GetString()=="experiment_set"&&!tool.GetProperty("annotations").GetProperty("readOnlyHint").GetBoolean()),"MCP experiment tool access annotations");
}
Check(McpText(McpCall("tools/call",new{},"experiment_status")).Contains("running"),"MCP experiment status snapshot");
Check(!mcpModel.Experiment().Running,"MCP experiment status does not start experiment");
Check(!McpText(McpCall("tools/call",new{action="start"},"experiment_set")).Contains("error")&&mcpModel.Experiment().Running,"MCP start uses application defaults");
Check(mcpModel.Experiment().DurationMs==5000&&mcpModel.Experiment().OnMs==200&&mcpModel.Experiment().PeriodMs==1000&&mcpModel.Experiment().Priority==2,"MCP experiment defaults match REST");
var runningBefore=JsonSerializer.Serialize(mcpModel.Experiment());
Check(McpText(McpCall("tools/call",new{action="start"},"experiment_set")).Contains("-32602")&&JsonSerializer.Serialize(mcpModel.Experiment())==runningBefore,"MCP already-running validation preserves state");
Check(!McpText(McpCall("tools/call",new{action="stop"},"experiment_set")).Contains("error")&&!mcpModel.Experiment().Running,"MCP stops existing experiment");
var stoppedBefore=JsonSerializer.Serialize(mcpModel.Experiment());
foreach(var invalid in new object[]{new{action="start",duration_ms=999},new{action="start",on_ms=1001,period_ms=1000},new{action="start",core=1},new{action="start",priority=6},new{action="start",kind="other"},new{action="start",duration_ms=1000.5},new{action="start",extra=true},new{action="start",password="not accepted"}})
 Check(McpText(McpCall("tools/call",invalid,"experiment_set")).Contains("-32602")&&JsonSerializer.Serialize(mcpModel.Experiment())==stoppedBefore,"MCP invalid experiment input preserves model state");
Check(!McpText(McpCall("tools/call",new{action="start",kind="pulse",duration_ms=1000,on_ms=100,period_ms=200},"experiment_set")).Contains("error"),"MCP starts pulse through existing model");
mcpModel.Advance(1000);Check(!mcpModel.Experiment().Running,"MCP experiment uses existing auto-completion");
var rgbDefaults=new DeviceModel();
Check(rgbDefaults.Strip().Enabled&&rgbDefaults.Strip().Driver=="rmt"&&rgbDefaults.Strip().Pixels.Length==10,"RGB startup enabled / RMT / ten LEDs");
Check(rgbDefaults.SetStrip(true,"solid",0x336699,10,1000,"spi")==null&&rgbDefaults.Strip().Driver=="spi"&&rgbDefaults.Strip().Pixels.All(p=>p==0x050a0f),"RGB SPI switch preserves scaled ten-pixel frame");
Check(rgbDefaults.SetStrip(true,"rainbow",0,20,150,"rmt")==null&&rgbDefaults.Strip().Driver=="rmt","RGB return to RMT");
Check(rgbDefaults.SetStrip(true,"solid",0,20,1000,"invalid")!=null&&rgbDefaults.Strip().Driver=="rmt","RGB invalid driver rejected without change");
Dictionary<string,object?> Map(object? v)=>(Dictionary<string,object?>)v!;
ulong U(object? v)=>Convert.ToUInt64(v);
var extension=new Dictionary<string,object?> { ["v"]=2,["not_registered_anywhere"]=new Dictionary<string,object?>{["температура"]=23.5,["samples"]=new object?[]{1,true,null,new byte[]{0,255}}}};
var extensionBytes=CborCodec.Encode(extension);
Check(extensionBytes.Contains((byte)'n')&&Map(Map(CborCodec.Decode(extensionBytes))["not_registered_anywhere"])["температура"] is double temperature&&temperature==23.5,"CBOR unknown UTF-8 names / nested values without registry");
var textMapReader=new CborReader(CborCodec.Encode(new{v=2,board_label="S3"}));textMapReader.ReadStartMap();Check(textMapReader.PeekState()==CborReaderState.TextString&&textMapReader.ReadTextString()=="v","wire key is actual text, not field ID");
if(args.Length==2&&args[0]=="--interop"){
 var dir=Path.GetFullPath(args[1]);Directory.CreateDirectory(dir);
 File.WriteAllBytes(Path.Combine(dir,"start.cbor"),CborCodec.Encode(new{v=2,action="start",kind="pi",duration_ms=5000,on_ms=200,period_ms=1000,core=0,priority=2}));
 File.WriteAllBytes(Path.Combine(dir,"strip.cbor"),CborCodec.Encode(new{v=2,enabled=true,mode="rainbow",color=0xffa030,brightness_pct=20,period_ms=1000}));
 File.WriteAllBytes(Path.Combine(dir,"echo.cbor"),CborCodec.Encode(new{v=2,type="echo.request",request_id="interop",data=new{bytes=new byte[]{0,1,128,255,0}}}));
 File.WriteAllBytes(Path.Combine(dir,"extension.cbor"),CborCodec.Encode(extension));
 var output=Path.Combine(dir,"device.cbor");if(File.Exists(output)){var map=Map(CborCodec.Decode(File.ReadAllBytes(output)));Check(U(map["iterations"])==ulong.MaxValue&& (double)map["pi"]! == Math.PI,"TinyCBOR -> .NET uint64 / double");Check(((byte[])Map(map["data"])["bytes"]!).SequenceEqual(new byte[]{0,1,128,255,0}),"TinyCBOR -> .NET binary/NUL");}
 Console.WriteLine("Interop fixtures ready: "+dir);return;
}
var zero=new Counters(0,0,0,0,0,0);
var a=new DeviceModel();var b=new DeviceModel();a.Apply(new("seed",Number:314));b.Apply(new("seed",Number:314));for(int i=0;i<25;i++){a.Advance(1000);b.Advance(1000);}
Check(JsonSerializer.Serialize(a.Summary(zero,0).Data)==JsonSerializer.Serialize(b.Summary(zero,0).Data),"seed/clock deterministic data");
var before=a.Tasks(0,8);a.Advance(1000);var after=a.Tasks(0,8);
Check(before.Data.Items.Select(t=>t.Id).SequenceEqual(after.Data.Items.Select(t=>t.Id)),"stable task IDs");
Check(after.Data.Items.Zip(before.Data.Items).All(t=>t.First.RuntimeTicks>=t.Second.RuntimeTicks),"monotonic runtime");
Check(after.Data.Items.Zip(before.Data.Items).All(t=>t.First.StackMinFreeBytes<=t.Second.StackMinFreeBytes),"stack watermark");
a.Apply(new("pause"));var time=a.State().TimeMs;a.Advance(1000);Check(time==a.State().TimeMs,"pause clock");a.Apply(new("step"));Check(a.State().TimeMs==time+1000,"exact step");
var oldBoot=a.State().BootId;a.Apply(new("reset"));Check(a.State().BootId!=oldBoot&&a.State().TimeMs==0,"reset identity");a.Apply(new("run"));
a.Apply(new("scenario","leak"));var original=a.Summary(zero,0).Data.Memory[0].FreeBytes;for(int i=0;i<40;i++)a.Advance(1000);Check(a.Summary(zero,0).Data.Memory[0].FreeBytes<original-30000,"memory leak scenario");
a.Apply(new("profile","s3"));a.Advance(1000);var first=a.Tasks(0,8);var second=a.Tasks(8,8,first.Data.SnapshotId);
Check(a.Info().Chip.Cores==2&&second.Data.Items.Length==4,"two-core model / pagination");Check(Math.Abs(first.Data.Items.Concat(second.Data.Items).Sum(t=>t.CpuPct??0)-100)<.1,"task shares sum to 100");
a.Advance(1000);try{a.Tasks(8,8,first.Data.SnapshotId);throw new Exception("stale accepted");}catch(InvalidOperationException){Check(true,"stale page rejected");}
Check(a.Apply(new("scenario","invalid"))!=null&&a.Apply(new("speed",Number:1000))!=null,"invalid model commands");for(int i=0;i<240;i++)a.Apply(new("pause"));Check(a.Events().Length==200,"bounded event log");
var m=new DeviceModel();
Check(m.SetExperiment("start","pi",5000,200,1000,0,2)==null,"start pi");Check(m.SetExperiment("start","pi",5000,200,1000,0,2)!=null,"one active experiment");
m.Advance(200);Check(m.Experiment().Iterations==0,"pi progress limited to 250 ms");m.Advance(100);Check(m.Experiment().Iterations>0&&Math.Abs(m.Experiment().Pi-Math.PI)<.001,"pi produces intermediate approximation");
for(int i=0;i<47;i++)m.Advance(100);Check(!m.Experiment().Running&&m.Experiment().ElapsedMs==5000&&m.Experiment().ProgressPct==100,"pi automatically finishes at 5 seconds");
Check(m.SetExperiment("start","pulse",5000,200,1000,0,3)==null,"start pulse 200/1000");var high=m.Summary(zero,0).Data.CpuBusyPct;m.Advance(200);var low=m.Summary(zero,0).Data.CpuBusyPct;Check(high-low>60&&!m.Experiment().Active,"pulse falling edge visible in CPU");m.Advance(800);Check(m.Experiment().Active,"pulse next rising edge");
var worker=m.Tasks(8,8).Data.Items.Single(t=>t.Name=="bench_worker");Check(worker.State=="running"&&worker.Priority==3&&worker.CoreAffinity==0,"worker state / priority / affinity");
m.SetExperiment("stop","pi",5000,200,1000,0,2);Check(!m.Experiment().Running&&m.Summary(zero,0).Data.CpuBusyPct<40,"stop clears active CPU load");
Check(m.SetExperiment("start","pi",999,200,1000,0,2)!=null&&m.SetExperiment("start","pi",5000,200,1000,1,2)!=null&&m.SetExperiment("start","pulse",5000,1001,1000,0,2)!=null,"experiment bounds / unavailable core");
Check(m.SetLed(true,20,0)==null&&!m.Led().Level,"LED minimum period / 0 percent");Check(m.SetLed(true,5000,100)==null&&m.Led().Level,"LED maximum period / 100 percent");m.SetLed(false,1000,50);Check(!m.Led().Level,"disabled LED");Check(m.SetLed(true,19,50)!=null&&m.SetLed(true,5001,50)!=null&&m.SetLed(true,1000,101)!=null,"LED bounds");
m.Apply(new("reset"));m.SetLed(true,1000,50);Check(m.Led().Level,"default LED on first half");m.Advance(500);Check(!m.Led().Level,"default LED off second half");m.Advance(500);Check(m.Led().Level,"default LED next cycle");
Check(m.SetStrip(true,"solid",0xff8040,100,1000)==null&&m.Strip().Pixels.All(p=>p==0xff8040),"RGB solid / 10 pixels");
m.SetStrip(true,"solid",0xffffff,0,20);Check(m.Strip().Pixels.All(p=>p==0),"RGB zero brightness");m.SetStrip(true,"chase",0xffffff,100,200);var pixel=Array.FindIndex(m.Strip().Pixels,p=>p!=0);m.Advance(200);Check(m.Strip().Pixels.Count(p=>p!=0)==1&&Array.FindIndex(m.Strip().Pixels,p=>p!=0)!=pixel,"RGB chase advances one pixel");
m.SetStrip(true,"rainbow",0,100,1000);Check(m.Strip().Pixels.Distinct().Count()>5,"RGB rainbow separate colors");m.SetStrip(false,"solid",0xffffff,100,1000);Check(m.Strip().Pixels.All(p=>p==0),"RGB disabled");Check(m.SetStrip(true,"invalid",0,20,1000)!=null&&m.SetStrip(true,"solid",0x1000000,20,1000)!=null&&m.SetStrip(true,"solid",0,101,1000)!=null,"RGB bounds");
var sample=CborCodec.Encode(new{v=2,pi=Math.PI,iterations=ulong.MaxValue,data=new{bytes=new byte[]{0,128,255}}});var decoded=CborCodec.DecodeMap(sample);
Check((double)decoded["pi"]! == Math.PI&&U(decoded["iterations"])==ulong.MaxValue,"CBOR exact double / native uint64");Check(((byte[])Map(decoded["data"])["bytes"]!).SequenceEqual(new byte[]{0,128,255}),"CBOR byte string");
foreach(var bad in new[]{new byte[]{0xa2,0x61,0x76,2,0x61,0x76,2},new byte[]{0xa0,0},new byte[]{0xbf,0xff},new byte[]{0xa1,0,2}}){try{CborCodec.DecodeMap(bad);throw new Exception("bad accepted");}catch(CborContentException){Check(true,"reject duplicate / trailing / indefinite / numeric keys");}}
Check(CborCodec.Encode(m.Summary(zero,4)).Length<=4096&&CborCodec.Encode(m.Tasks(0,8)).Length<=4096,"telemetry within frame budget");
if(args.Length==2&&args[0]=="--url"){
 var baseUrl=args[1].TrimEnd('/');if(!Uri.TryCreate(baseUrl,UriKind.Absolute,out var uri)||!IPAddress.TryParse(uri.Host,out var ip)||!IPAddress.IsLoopback(ip))throw new Exception("Tests require numeric loopback");
 using var client=new HttpClient{BaseAddress=uri,Timeout=TimeSpan.FromSeconds(8)};
 async Task<JsonElement> ReadDiagnostic(HttpResponseMessage response){
   Check(response.IsSuccessStatusCode&&response.Content.Headers.ContentType?.MediaType=="text/event-stream","SSE content type / status");
   using var timeout=new CancellationTokenSource(TimeSpan.FromSeconds(5));
   using var reader=new StreamReader(await response.Content.ReadAsStreamAsync(),Encoding.UTF8,leaveOpen:true);
   string? line;bool named=false;string? data=null,id=null;
   while((line=await reader.ReadLineAsync(timeout.Token)) is not null){
     if(line=="event: diagnostic")named=true;
     if(line.StartsWith("id: "))id=line[4..];
     if(line.StartsWith("data: "))data=line[6..];
     if(line.Length==0&&data is not null)break;
   }
   Check(named&&id is not null&&data is not null,"SSE named event / id / data");
   using var json=JsonDocument.Parse(data!);var root=json.RootElement;
   Check(root.GetProperty("v").GetInt32()==2&&root.GetProperty("source").GetString()=="simulator"&&root.GetProperty("free_bytes").GetInt32()>0,"SSE diagnostic payload / source");
   return root.Clone();
 }
 async Task<HttpResponseMessage> McpHttp(string method,object? arguments=null,string name="echo"){
 using var request=new HttpRequestMessage(HttpMethod.Post,"/mcp");
 request.Headers.Add("Accept","application/json,text/event-stream");request.Headers.Add("MCP-Protocol-Version",Showcase.Host.SimulatorMcp.Version);request.Headers.Add("Mcp-Method",method);
 if(method=="tools/call")request.Headers.Add("Mcp-Name",name);
 request.Content=new ByteArrayContent(McpRequest(method,arguments,name));request.Content.Headers.ContentType=new("application/json");
 return await client.SendAsync(request);
}
using(var deniedMcp=await client.PostAsync("/mcp",new StringContent("{",Encoding.UTF8,"application/json")))Check(deniedMcp.StatusCode==HttpStatusCode.Unauthorized&&deniedMcp.Headers.WwwAuthenticate.Any(header=>header.Scheme=="Basic"),"MCP missing credentials rejected before malformed JSON parse");
 using(var denied=await client.GetAsync("/api/v2/auth/status"))Check(denied.StatusCode==HttpStatusCode.Unauthorized&&denied.Headers.WwwAuthenticate.Any(h=>h.Scheme=="Basic"),"auth probe missing credentials / challenge");
 using(var denied=await client.GetAsync("/api/v2/events/private"))Check(denied.StatusCode==HttpStatusCode.Unauthorized,"SSE denies before stream starts");
 ByteArrayContent Body(object value){var c=new ByteArrayContent(CborCodec.Encode(value));c.Headers.ContentType=new(CborCodec.MediaType);return c;}
 async Task<object?> Get(string path){using var res=await client.GetAsync(path);res.EnsureSuccessStatusCode();if(res.Content.Headers.ContentType?.MediaType!=CborCodec.MediaType)throw new Exception("CBOR required");return CborCodec.Decode(await res.Content.ReadAsByteArrayAsync());}
 async Task<HttpResponseMessage> Post(string path,object value)=>await client.PostAsync(path,Body(value));
 Check(Map(await Get("/api/v2/info"))["source"] as string=="simulator","live simulator / CBOR");await Post("/sim/api/v2/command",new{v=2,action="reset"});
 Check(U(Map(await Get("/api/v2/summary"))["v"])==2,"summary v2");
 var configuredAssetRoot=Environment.GetEnvironmentVariable("NAWSERVER_TEST_ASSET_ROOT");
 if(!string.IsNullOrEmpty(configuredAssetRoot)&&!Path.IsPathFullyQualified(configuredAssetRoot))throw new Exception("NAWSERVER_TEST_ASSET_ROOT requires an absolute path");
 var assetRoot=Path.GetFullPath(configuredAssetRoot??Path.Combine(AppContext.BaseDirectory,"../../../../../frontend/dist"));using(var manifest=JsonDocument.Parse(File.ReadAllBytes(Path.Combine(assetRoot,"asset-manifest.json")))){
 foreach(var entry in manifest.RootElement.GetProperty("files").EnumerateArray()){
 var name=entry.GetProperty("path").GetString()!;var route=name=="index.html"?"/":"/"+name;var identity=await client.GetByteArrayAsync(route);Check(Convert.ToHexStringLower(SHA256.HashData(identity))==entry.GetProperty("sha256").GetString(),"asset identity "+name);
 using var req=new HttpRequestMessage(HttpMethod.Get,route);req.Headers.AcceptEncoding.Add(new StringWithQualityHeaderValue("gzip"));using var res=await client.SendAsync(req);Check(res.Content.Headers.ContentEncoding.Contains("gzip")&&Convert.ToHexStringLower(SHA256.HashData(await res.Content.ReadAsByteArrayAsync()))==entry.GetProperty("gzip_sha256").GetString(),"gzip identity "+name);
 }}
 Check((await Post("/api/v2/led",new{v=2,enabled=true,period_ms=1000,duty_pct=50})).StatusCode==HttpStatusCode.Unauthorized,"LED protected");
 client.DefaultRequestHeaders.Authorization=new("Basic",Convert.ToBase64String(Encoding.UTF8.GetBytes("demo:wrong")));Check((await Post("/api/v2/experiments",new{v=2,action="stop"})).StatusCode==HttpStatusCode.Unauthorized,"experiment wrong credential");
 using(var deniedMcp=await McpHttp("tools/call",new{action="start"},"experiment_set"))Check(deniedMcp.StatusCode==HttpStatusCode.Unauthorized,"MCP wrong credential cannot start experiment");
 client.DefaultRequestHeaders.Authorization=new("Basic",Convert.ToBase64String(Encoding.UTF8.GetBytes("demo:showcase-only")));
 using(var discoveredMcp=await McpHttp("server/discover"))Check(discoveredMcp.IsSuccessStatusCode,"MCP authenticated discovery");
 using(var startedMcp=await McpHttp("tools/call",new{action="start"},"experiment_set"))Check(startedMcp.IsSuccessStatusCode&&!Encoding.UTF8.GetString(await startedMcp.Content.ReadAsByteArrayAsync()).Contains("error"),"MCP authenticated experiment starts");
 Check(Map(await Get("/api/v2/experiments"))["running"] is true,"MCP start visible through existing REST state");
 using(var stoppedMcp=await McpHttp("tools/call",new{action="stop"},"experiment_set"))Check(stoppedMcp.IsSuccessStatusCode,"MCP authenticated experiment stops");
 Check(Map(await Get("/api/v2/experiments"))["running"] is false,"MCP stop visible through existing REST state");
 Check((await client.GetAsync("/api/v2/auth/status")).StatusCode==HttpStatusCode.OK,"auth probe correct credentials");
 Check((await client.GetAsync("/api/v2/auth/admin")).StatusCode==HttpStatusCode.Forbidden,"auth probe insufficient permissions");
 using(var privateEvents=await client.GetAsync("/api/v2/events/private",HttpCompletionOption.ResponseHeadersRead))await ReadDiagnostic(privateEvents);
 Check((await Post("/api/v2/led",new{v=2,enabled=true,period_ms=20,duty_pct=100})).IsSuccessStatusCode,"LED CBOR applies");Check(U(Map(await Get("/api/v2/led"))["period_ms"])==20,"LED readback");
 Check((await Post("/api/v2/led",new{v=2,enabled=true,period_ms=19,duty_pct=100})).StatusCode==HttpStatusCode.BadRequest,"LED bad period rejected");
 Check((await Post("/api/v2/experiments",new{v=2,action="start",kind="pi",duration_ms=1000})).IsSuccessStatusCode,"HTTP start pi");
 await Task.Delay(1400);Check(Map(await Get("/api/v2/experiments"))["running"] is false,"HTTP pi auto completion");
 Check((await Post("/api/v2/experiments",new{v=2,action="start",kind="pulse",duration_ms=5000,on_ms=200,period_ms=1000})).IsSuccessStatusCode,"HTTP start pulse");Check((await Post("/api/v2/experiments",new{v=2,action="stop"})).IsSuccessStatusCode,"HTTP stop pulse");
 Check((await Post("/api/v2/strip",new{v=2,enabled=true,mode="rainbow",color=0xff0000,brightness_pct=30,period_ms=1000})).IsSuccessStatusCode,"RGB protected CBOR control");Check(((object?[])Map(await Get("/api/v2/strip"))["pixels"]!).Length==10,"RGB HTTP ten pixels");await Post("/api/v2/strip",new{v=2,enabled=false,mode="solid",color=0xffa030,brightness_pct=20,period_ms=1000});
 Check((await client.PostAsync("/api/v2/led",new StringContent("{}",Encoding.UTF8,"application/json"))).StatusCode==HttpStatusCode.UnsupportedMediaType,"JSON commands rejected");
 Check((await Post("/api/v2/led",new{enabled=true,period_ms=1000,duty_pct=50})).StatusCode==HttpStatusCode.BadRequest,"version required");
 Check((await client.PostAsync("/api/v2/led",new ByteArrayContent(new byte[]{0xa2,0x61,0x76,2,0x61,0x76,2}){Headers={ContentType=new(CborCodec.MediaType)}})).StatusCode==HttpStatusCode.BadRequest,"malformed CBOR HTTP rejected");
 var huge=new ByteArrayContent(new byte[4097]);huge.Headers.ContentType=new(CborCodec.MediaType);Check((await client.PostAsync("/api/v2/led",huge)).StatusCode==HttpStatusCode.RequestEntityTooLarge,"HTTP body ceiling");
 using(var cross=new HttpRequestMessage(HttpMethod.Post,"/sim/api/v2/command")){cross.Headers.Add("Origin","http://foreign.invalid");cross.Content=Body(new{v=2,action="pause"});Check((await client.SendAsync(cross)).StatusCode==HttpStatusCode.Forbidden,"foreign Origin rejected");}
 Check((await client.GetAsync("/api/v2/tasks?limit=100")).StatusCode==HttpStatusCode.BadRequest,"task page ceiling");Check((await client.GetAsync("/api/v1/info")).StatusCode==HttpStatusCode.NotFound,"no legacy JSON API");
 var text="Привет & + <script> ✓";Check(await client.GetStringAsync("/api/v2/echo?value="+Uri.EscapeDataString(text))==text,"explicit text teaching route");
 using var ws=new ClientWebSocket();await ws.ConnectAsync(new Uri(baseUrl.Replace("http://","ws://")+"/ws"),CancellationToken.None);
 using var publicEvents=await client.GetAsync("/api/v2/events",HttpCompletionOption.ResponseHeadersRead);
 await ReadDiagnostic(publicEvents);
 async Task<Dictionary<string,object?>> Receive(string type){using var deadline=new CancellationTokenSource(TimeSpan.FromSeconds(6));while(true){using var stream=new MemoryStream();var buf=new byte[8192];WebSocketReceiveResult r;do{r=await ws.ReceiveAsync(buf,deadline.Token);if(r.MessageType==WebSocketMessageType.Close)throw new Exception("unexpected close");stream.Write(buf,0,r.Count);}while(!r.EndOfMessage);if(r.MessageType!=WebSocketMessageType.Binary)throw new Exception("binary CBOR required");var d=CborCodec.DecodeMap(stream.ToArray());if(d.GetValueOrDefault("type") as string==type)return d;}}
 Check(Map((await Receive("summary"))["data"])["experiment"] is Dictionary<string,object?>,"WS CBOR telemetry");
 foreach(int size in new[]{0,1,127,128,256,3800}){var bytes=Enumerable.Range(0,size).Select(i=>(byte)(i%256)).ToArray();await ws.SendAsync(CborCodec.Encode(new{v=2,type="echo.request",request_id="bytes",data=new{bytes}}),WebSocketMessageType.Binary,true,CancellationToken.None);Check(((byte[])Map((await Receive("echo.response"))["data"])["bytes"]!).SequenceEqual(bytes),$"CBOR WS byte string {size}");}
 foreach(var value in new[]{"","Привет 🌍","<script>&+"}){await ws.SendAsync(CborCodec.Encode(new{v=2,type="echo.request",request_id="text",data=new{text=value}}),WebSocketMessageType.Binary,true,CancellationToken.None);Check(Map((await Receive("echo.response"))["data"])["text"] as string==value,"CBOR WS Unicode text");}
 await ws.SendAsync(new byte[]{0xff},WebSocketMessageType.Binary,true,CancellationToken.None);Check((await Receive("error"))["type"] as string=="error","malformed CBOR WS error");
 await ws.SendAsync(new byte[4097],WebSocketMessageType.Binary,true,CancellationToken.None);bool closed=false;try{await Receive("error");}catch(Exception e)when(e is WebSocketException||e.Message=="unexpected close"){closed=true;}Check(closed,"oversized WS closes");
 Check((await client.GetAsync("/health")).IsSuccessStatusCode,"healthy after rejected input");
 await Post("/api/v2/led",new{v=2,enabled=true,period_ms=1000,duty_pct=50});await Post("/sim/api/v2/command",new{v=2,action="reset"});
}
Console.WriteLine($"RESULT: {passed} checks passed");

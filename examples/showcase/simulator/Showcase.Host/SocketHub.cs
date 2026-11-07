using System.Collections.Concurrent;
using System.Net.WebSockets;
using System.Text;
using System.Formats.Cbor;
using System.Threading.Channels;
using Showcase.Contracts;
using Showcase.Model;
namespace Showcase.Host;
public sealed class Traffic {
    public long Http, RxFrames, TxFrames, RxBytes, TxBytes, Rejected;
    public Counters Snapshot()=>new((ulong)Interlocked.Read(ref Http),(ulong)Interlocked.Read(ref RxFrames),(ulong)Interlocked.Read(ref TxFrames),(ulong)Interlocked.Read(ref RxBytes),(ulong)Interlocked.Read(ref TxBytes),(ulong)Interlocked.Read(ref Rejected));
}
public sealed class SocketHub(Traffic traffic) {
    private sealed record Frame(WebSocketMessageType Type, byte[] Data);
    private sealed class Client(string id,WebSocket socket,long connectedMs) {
        public string Id=id; public WebSocket Socket=socket; public long ConnectedMs=connectedMs, Rx, Tx;
        public Channel<Frame> Queue=Channel.CreateBounded<Frame>(new BoundedChannelOptions(4){SingleReader=true,FullMode=BoundedChannelFullMode.Wait});
    }
    private readonly ConcurrentDictionary<string,Client> clients=new();
    private readonly object admission=new(); private long nextId;
    public int Count=>clients.Count;
    public Showcase.Contracts.ConnectionInfo[] Connections()=>clients.Values.Select(c=>new Showcase.Contracts.ConnectionInfo(c.Id,c.ConnectedMs,c.Socket.State.ToString(),(ulong)Interlocked.Read(ref c.Rx),(ulong)Interlocked.Read(ref c.Tx))).OrderBy(c=>c.Id).ToArray();
    private void Offer(Client c,Frame frame){if(!c.Queue.Writer.TryWrite(frame)){Interlocked.Increment(ref traffic.Rejected);c.Socket.Abort();}}
    public void Broadcast<T>(T payload){var frame=new Frame(WebSocketMessageType.Binary,CborCodec.Encode(payload));if(frame.Data.Length>4096)throw new InvalidOperationException("Summary exceeds frame budget");foreach(var c in clients.Values)Offer(c,frame);}
    public void Disconnect(){foreach(var c in clients.Values)c.Socket.Abort();}
    public async Task Handle(HttpContext context,DeviceModel model){
        if(!context.WebSockets.IsWebSocketRequest){await CborHttp.Error("cbor_subprotocol_required",400).ExecuteAsync(context);return;}
        // Reserve a slot before the asynchronous handshake.
        string id;
        lock(admission){if(clients.Count+reservations>=4){context.Response.StatusCode=503;Interlocked.Increment(ref traffic.Rejected);return;}reservations++;id=$"ws-{Interlocked.Increment(ref nextId)}";}
        WebSocket socket;
        try{socket=await context.WebSockets.AcceptWebSocketAsync();}catch{lock(admission)reservations--;throw;}
        using var socketLifetime=socket;
        var client=new Client(id,socket,model.State().TimeMs);
        lock(admission){clients[id]=client;reservations--;}
        using var cancel=CancellationTokenSource.CreateLinkedTokenSource(context.RequestAborted);
        var sending=Send(client,cancel.Token);
        try{
            var buffer=new byte[4097];
            while(socket.State==WebSocketState.Open){
                int count=0;WebSocketReceiveResult result;
                do{
                    result=await socket.ReceiveAsync(new ArraySegment<byte>(buffer,count,buffer.Length-count),cancel.Token);
                    if(result.MessageType==WebSocketMessageType.Close){client.Queue.Writer.TryComplete();return;}
                    count+=result.Count;if(count>4096){Interlocked.Increment(ref traffic.Rejected);socket.Abort();return;}
                }while(!result.EndOfMessage);
                Interlocked.Increment(ref client.Rx);Interlocked.Increment(ref traffic.RxFrames);Interlocked.Add(ref traffic.RxBytes,count);
                if(result.MessageType!=WebSocketMessageType.Binary){Interlocked.Increment(ref traffic.Rejected);socket.Abort();return;}
                try{
                    var root=CborCodec.DecodeMap(buffer.AsMemory(0,count));
                    if(root.GetValueOrDefault("v") is not ulong v||v!=2||root.GetValueOrDefault("type") as string!="echo.request")throw new CborContentException("version/type");
                    if(root.GetValueOrDefault("request_id") is not string requestId||requestId.Length>32||root.GetValueOrDefault("data") is not Dictionary<string,object?> data)throw new CborContentException("envelope");
                    bool text=data.GetValueOrDefault("text") is string, binary=data.GetValueOrDefault("bytes") is byte[];
                    if(text==binary)throw new CborContentException("one payload type required");
                    object replyData=text?new{text=(string)data["text"]!}:new{bytes=(byte[])data["bytes"]!};
                    var reply=CborCodec.Encode(new{v=2,type="echo.response",request_id=requestId,data=replyData});if(reply.Length>4096)throw new CborContentException("reply limit");
                    Offer(client,new(WebSocketMessageType.Binary,reply));
                }catch(CborContentException){Offer(client,new(WebSocketMessageType.Binary,CborCodec.Encode(new{v=2,type="error",error=new{code="invalid_message"}})));}
            }
        }catch(Exception e)when(e is WebSocketException or OperationCanceledException or IOException){}
        finally{clients.TryRemove(id,out _);client.Queue.Writer.TryComplete();cancel.Cancel();socket.Abort();try{await sending;}catch(OperationCanceledException){}}
    }
    private int reservations;
    private async Task Send(Client c,CancellationToken token){try{await foreach(var f in c.Queue.Reader.ReadAllAsync(token)){using var deadline=CancellationTokenSource.CreateLinkedTokenSource(token);deadline.CancelAfter(TimeSpan.FromSeconds(3));await c.Socket.SendAsync(f.Data.AsMemory(),f.Type,true,deadline.Token);Interlocked.Increment(ref c.Tx);Interlocked.Increment(ref traffic.TxFrames);Interlocked.Add(ref traffic.TxBytes,f.Data.Length);}}catch(Exception e)when(e is WebSocketException or OperationCanceledException or IOException){c.Socket.Abort();}}
}
public sealed class SimulationWorker(DeviceModel model,SocketHub sockets,Traffic traffic):BackgroundService {
    protected override async Task ExecuteAsync(CancellationToken token){using var timer=new PeriodicTimer(TimeSpan.FromMilliseconds(50));int sample=0;while(await timer.WaitForNextTickAsync(token)){model.Advance(50);sample++;if(model.NetworkOffline){sockets.Disconnect();continue;}if(sample%(model.FastTelemetry?2:20)!=0)continue;var snapshot=model.Summary(traffic.Snapshot(),sockets.Count);sockets.Broadcast(new{v=2,type="summary",source=snapshot.Source,boot_id=snapshot.BootId,seq=snapshot.Seq,captured_ms=snapshot.CapturedMs,data=snapshot.Data});}}
}



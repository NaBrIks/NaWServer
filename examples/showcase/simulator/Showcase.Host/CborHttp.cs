using Showcase.Contracts;
namespace Showcase.Host;
public sealed class CborResult(object? value,int status=200):IResult {
    public async Task ExecuteAsync(HttpContext context){var bytes=CborCodec.Encode(value);context.Response.StatusCode=status;context.Response.ContentType=CborCodec.MediaType;context.Response.ContentLength=bytes.Length;await context.Response.Body.WriteAsync(bytes,context.RequestAborted);}
}
public static class CborHttp {
    public static IResult Reply(object? value,int status=200)=>new CborResult(value,status);
    public static IResult Error(string code,int status,string? message=null)=>Reply(new{v=2,error=new{code,message}},status);
    public static bool IsCbor(HttpRequest r)=>r.ContentType?.Split(';')[0].Trim().Equals(CborCodec.MediaType,StringComparison.OrdinalIgnoreCase)==true;
    public static async Task<Dictionary<string,object?>> Body(HttpContext context){using var stream=new MemoryStream();var buffer=new byte[1024];while(true){var count=await context.Request.Body.ReadAsync(buffer,context.RequestAborted);if(count==0)break;if(stream.Length+count>4096)throw new BadHttpRequestException("Body limit",413);stream.Write(buffer,0,count);}return CborCodec.DecodeMap(stream.ToArray());}
}

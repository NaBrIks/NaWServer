using System.Net.Http.Headers;
namespace Showcase.Host;
public static class SimulatorMcpHttp
{
    public static async Task Handle(HttpContext context,SimulatorMcp endpoint){
        var request=context.Request;var response=context.Response;
        response.ContentType="application/json";
        if(!HttpMethods.IsPost(request.Method)){response.StatusCode=405;response.Headers.Allow="POST";return;}
        var origin=request.Headers.Origin;
        if(origin.Count>1||(origin.Count==1&&origin[0]!=$"http://{context.Connection.LocalIpAddress}:{context.Connection.LocalPort}")){response.StatusCode=403;return;}
        if(!MediaTypeHeaderValue.TryParse(request.ContentType,out var contentType)||!string.Equals(contentType.MediaType,"application/json",StringComparison.OrdinalIgnoreCase)){response.StatusCode=415;return;}
        try{var accepts=request.GetTypedHeaders().Accept;if(accepts is null||!accepts.Any(value=>value.MediaType.Value?.Equals("application/json",StringComparison.OrdinalIgnoreCase)==true&&(value.Quality??1)>0)){response.StatusCode=406;return;}}
        catch(FormatException){response.StatusCode=406;return;}
        string[] names=["MCP-Protocol-Version","Mcp-Method","Mcp-Name"];
        var invalidHeaders=names.Any(name=>request.Headers[name].Count>1);
        var input=new byte[4097];int length=0;
        try{while(length<input.Length){int count=await request.Body.ReadAsync(input.AsMemory(length),context.RequestAborted);if(count==0)break;length+=count;}}catch(BadHttpRequestException){response.StatusCode=413;return;}
        var reply=endpoint.Dispatch(input.AsMemory(0,length),request.Headers[names[0]].ToString(),request.Headers[names[1]].ToString(),request.Headers[names[2]].ToString(),invalidHeaders);
        response.StatusCode=reply.Status;response.ContentLength=reply.Body.Length;await response.Body.WriteAsync(reply.Body,context.RequestAborted);
    }
}

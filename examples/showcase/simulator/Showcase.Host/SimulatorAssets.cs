namespace Showcase.Host;
public static class SimulatorAssets
{
    public static void Map(WebApplication app,string directory){
        var root=Path.GetFullPath(directory);
        var assetRoot=Path.Combine(root,"assets")+Path.DirectorySeparatorChar;
        var types=new Microsoft.AspNetCore.StaticFiles.FileExtensionContentTypeProvider();
        IResult Serve(HttpContext context,string path){
            if(!File.Exists(path))return CborHttp.Error("not_found",404);
            var mime=types.TryGetContentType(path,out var value)?value:"application/octet-stream";
            bool gzip=context.Request.GetTypedHeaders().AcceptEncoding?.Any(item=>item.Value=="gzip"&&(item.Quality??1)>0)==true&&File.Exists(path+".gz");
            context.Response.Headers.Vary="Accept-Encoding";
            if(gzip)context.Response.Headers.ContentEncoding="gzip";
            return Results.File(gzip?path+".gz":path,mime);
        }
        // Resolve only the shared frontend's index and assets tree on each request.
        // Vue build hashes may change while the simulator runs. API/MCP paths are
        // never candidates, and no directory enumeration or traversal is exposed.
        app.MapMethods("/",["GET","HEAD"],(HttpContext context)=>Serve(context,Path.Combine(root,"index.html")));
        app.MapMethods("/licenses/Tabler-Icons.txt",["GET","HEAD"],(HttpContext context)=>Serve(context,Path.Combine(root,"licenses","Tabler-Icons.txt")));
        app.MapMethods("/assets/{**asset}",["GET","HEAD"],(HttpContext context,string? asset)=>{
            if(string.IsNullOrEmpty(asset)||asset.EndsWith(".gz",StringComparison.OrdinalIgnoreCase)||asset.Contains('\\')||asset.Split('/').Any(part=>part is "." or ".."))return CborHttp.Error("not_found",404);
            string path;
            try{path=Path.GetFullPath(Path.Combine(assetRoot,asset));}catch(ArgumentException){return CborHttp.Error("not_found",404);}
            var comparison=OperatingSystem.IsWindows()?StringComparison.OrdinalIgnoreCase:StringComparison.Ordinal;
            return path.StartsWith(assetRoot,comparison)?Serve(context,path):CborHttp.Error("not_found",404);
        });
    }
}

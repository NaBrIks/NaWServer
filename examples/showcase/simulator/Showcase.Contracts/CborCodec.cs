using System.Collections;
using System.Collections.Concurrent;
using System.Formats.Cbor;
using System.Reflection;
using System.Text.Json;
namespace Showcase.Contracts;

// Application CBOR codec. Maps carry UTF-8 field names directly; no registry.
public static class CborCodec {
    public const string MediaType="application/cbor";
    public const string Subprotocol="nawserver.cbor.v2";
    private static readonly ConcurrentDictionary<Type,PropertyInfo[]> Properties=new();
    public static byte[] Encode(object? value){var writer=new CborWriter(CborConformanceMode.Strict);Write(writer,value,0);return writer.Encode();}
    private static void Write(CborWriter w,object? value,int depth){
        if(depth>12)throw new CborContentException("Depth limit");
        switch(value){
            case null:w.WriteNull();return;case bool b:w.WriteBoolean(b);return;case byte[] bytes:w.WriteByteString(bytes);return;case string s:w.WriteTextString(s);return;
            case byte n:w.WriteUInt64(n);return;case ushort n:w.WriteUInt64(n);return;case uint n:w.WriteUInt64(n);return;case ulong n:w.WriteUInt64(n);return;
            case sbyte n:w.WriteInt64(n);return;case short n:w.WriteInt64(n);return;case int n:w.WriteInt64(n);return;case long n:w.WriteInt64(n);return;
            case float n:if(!float.IsFinite(n))throw new CborContentException("Nonfinite value");w.WriteSingle(n);return;
            case double n:if(!double.IsFinite(n))throw new CborContentException("Nonfinite value");w.WriteDouble(n);return;
            case IDictionary map:w.WriteStartMap(map.Count);foreach(DictionaryEntry entry in map){WriteKey(w,(string)entry.Key);Write(w,entry.Value,depth+1);}w.WriteEndMap();return;
            case IEnumerable array:var items=array.Cast<object?>().ToArray();w.WriteStartArray(items.Length);foreach(var item in items)Write(w,item,depth+1);w.WriteEndArray();return;
        }
        var props=Properties.GetOrAdd(value.GetType(),t=>t.GetProperties(BindingFlags.Public|BindingFlags.Instance).Where(p=>p.CanRead&&p.GetIndexParameters().Length==0).ToArray());
        w.WriteStartMap(props.Length);foreach(var p in props){WriteKey(w,JsonNamingPolicy.SnakeCaseLower.ConvertName(p.Name));Write(w,p.GetValue(value),depth+1);}w.WriteEndMap();
    }
    private static void WriteKey(CborWriter writer,string name)=>writer.WriteTextString(name);
    public static object? Decode(ReadOnlyMemory<byte> bytes,int maxBytes=65536){if(bytes.Length>maxBytes)throw new CborContentException("Byte limit");var reader=new CborReader(bytes,CborConformanceMode.Strict);var result=Read(reader,0);if(reader.BytesRemaining!=0)throw new CborContentException("Trailing item");return result;}
    public static Dictionary<string,object?> DecodeMap(ReadOnlyMemory<byte> bytes,int maxBytes=4096)=>Decode(bytes,maxBytes) as Dictionary<string,object?>??throw new CborContentException("Root map required");
    private static object? Read(CborReader r,int depth){
        if(depth>12)throw new CborContentException("Depth limit");
        switch(r.PeekState()){
            case CborReaderState.UnsignedInteger:return r.ReadUInt64();case CborReaderState.NegativeInteger:return r.ReadInt64();case CborReaderState.TextString:return r.ReadTextString();case CborReaderState.ByteString:return r.ReadByteString();case CborReaderState.Boolean:return r.ReadBoolean();case CborReaderState.Null:r.ReadNull();return null;
            case CborReaderState.HalfPrecisionFloat:case CborReaderState.SinglePrecisionFloat:case CborReaderState.DoublePrecisionFloat:double n=r.ReadDouble();if(!double.IsFinite(n))throw new CborContentException("Nonfinite value");return n;
            case CborReaderState.StartArray:var count=r.ReadStartArray();if(count is null||count>256)throw new CborContentException("Array limit");var array=new object?[count.Value];for(int i=0;i<array.Length;i++)array[i]=Read(r,depth+1);r.ReadEndArray();return array;
            case CborReaderState.StartMap:var length=r.ReadStartMap();if(length is null||length>128)throw new CborContentException("Map limit");var map=new Dictionary<string,object?>();for(int i=0;i<length;i++){if(r.PeekState()!=CborReaderState.TextString)throw new CborContentException("Text field name required");string key=r.ReadTextString();if(!map.TryAdd(key,Read(r,depth+1)))throw new CborContentException("Duplicate key");}r.ReadEndMap();return map;
            default:throw new CborContentException("Unsupported CBOR type");
        }
    }
}


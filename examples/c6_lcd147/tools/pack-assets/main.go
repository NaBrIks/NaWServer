package main

import (
 "bytes"
 "compress/gzip"
 "crypto/sha256"
 "encoding/json"
 "fmt"
 "os"
 "path/filepath"
 "sort"
 "strings"
)

func main() { if err:=pack();err!=nil {fmt.Fprintln(os.Stderr,err);os.Exit(1)} }
func pack() error {
 root,err:=os.Getwd();if err!=nil{return err}; dist:=filepath.Join(root,"frontend","dist")
 var paths []string
 err=filepath.WalkDir(dist,func(path string,entry os.DirEntry,walkErr error)error{
  if walkErr!=nil{return walkErr};if entry.IsDir(){return nil};rel,e:=filepath.Rel(dist,path);if e!=nil{return e}
  rel=filepath.ToSlash(rel);if strings.HasPrefix(rel,"assets/SimulatorPanel-")||strings.HasSuffix(rel,"manifest.json"){return nil};paths=append(paths,rel);return nil
 });if err!=nil{return err};sort.Strings(paths)
 var header bytes.Buffer;fmt.Fprintln(&header,"#pragma once\n#include <cstddef>\n#include <cstdint>\nnamespace c6_assets {")
 type asset struct{Path string `json:"path"`;SHA string `json:"sha256"`;Bytes int `json:"bytes"`}
 var manifest []asset;var rows []string
 for index,path:=range paths {
  raw,e:=os.ReadFile(filepath.Join(dist,filepath.FromSlash(path)));if e!=nil{return e};digest:=sha256.Sum256(raw)
  manifest=append(manifest,asset{path,fmt.Sprintf("%x",digest),len(raw)})
  var compressed bytes.Buffer;zip,_:=gzip.NewWriterLevel(&compressed,gzip.BestCompression);if _,e=zip.Write(raw);e!=nil{return e};if e=zip.Close();e!=nil{return e}
  fmt.Fprintf(&header,"inline constexpr uint8_t asset_%d[]={",index);for i,b:=range compressed.Bytes(){if i%32==0{fmt.Fprint(&header,"\n")};fmt.Fprintf(&header,"0x%02x,",b)};fmt.Fprintln(&header,"\n};")
  mime:="application/octet-stream";switch filepath.Ext(path){case ".html":mime="text/html; charset=utf-8";case ".js":mime="text/javascript; charset=utf-8";case ".css":mime="text/css; charset=utf-8";case ".woff":mime="font/woff";case ".txt":mime="text/plain; charset=utf-8"}
  uri:="/"+path;if path=="index.html"{uri="/"};rows=append(rows,fmt.Sprintf("{%q,%q,asset_%d,sizeof(asset_%d)},",uri,mime,index,index))
 }
 fmt.Fprintln(&header,"struct Asset{const char *path;const char *mime;const uint8_t *data;size_t length;};\ninline constexpr Asset files[]={");for _,row:=range rows{fmt.Fprintln(&header,row)};fmt.Fprintln(&header,"};\n}")
 if err=os.WriteFile(filepath.Join(root,"main","assets.hpp"),header.Bytes(),0644);err!=nil{return err}
 contents,err:=json.MarshalIndent(struct{Files []asset `json:"files"`}{manifest},"","  ");if err!=nil{return err}
 if err=os.WriteFile(filepath.Join(dist,"device-manifest.json"),contents,0644);err!=nil{return err}
 fmt.Printf("Embedded %d local device assets\n",len(paths));return nil
}


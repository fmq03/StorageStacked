/* Exercise the installed extensions' real definition providers with an editor
 * API shim. The SystemVerilog parser and ctagsx lookup are used unchanged. */
const fs=require('fs'),path=require('path'),Module=require('module');
const ROOT=path.resolve(__dirname,'..');
const EXT=path.join(process.env.HOME,'.vscode-server/extensions');
class Position {
 constructor(line,character){this.line=line;this.character=character;}
 isBeforeOrEqual(p){return this.line<p.line || this.line===p.line&&this.character<=p.character;}
 isAfter(p){return !this.isBeforeOrEqual(p);}
 isEqual(p){return this.line===p.line&&this.character===p.character;}
}
class Range {
 constructor(a,b,c,d){if(typeof a==='number'){this.start=new Position(a,b);this.end=new Position(c,d);}else{this.start=a;this.end=b;}}
}
class Location {constructor(uri,range){this.uri=uri;this.range=range instanceof Position?new Range(range,range):range;}}
class SymbolInformation {constructor(name,kind,containerName,location){Object.assign(this,{name,kind,containerName,location});}}
const uri=file=>({fsPath:file,path:file,scheme:'file',toString:()=>new URL('file://'+file).href});
const defaults={'systemverilog.documentSymbolsPrecision':'full','ctagsx.disableDefinitionProvider':false};
let scalaProvider, workspaceProvider, documentProvider;
const config={get:k=>defaults[k]??false};
const shim={Position,Range,Selection:Range,Location,SymbolInformation,Uri:{file:uri},workspace:{rootPath:ROOT,getConfiguration:()=>config},window:{},
 languages:{registerDefinitionProvider:(_selector,p)=>{scalaProvider=p;return {dispose(){}};}},
 commands:{registerCommand:()=>({dispose(){}}),executeCommand:async(name,...args)=>{
   if(name==='vscode.executeWorkspaceSymbolProvider')return workspaceProvider.provideWorkspaceSymbols(...args);
   if(name==='vscode.executeDocumentSymbolProvider')return documentProvider.provideDocumentSymbols(document(args[0].fsPath),{isCancellationRequested:false});
   throw Error(name);
 }}};
function document(file){
 const text=fs.readFileSync(file,'utf8'),lines=text.split('\n');
 const doc={uri:uri(file),fileName:file,isUntitled:false,languageId:file.endsWith('.scala')?'scala':'systemverilog',lineCount:lines.length,
 lineAt:n=>({text:lines[n]}),positionAt:n=>{let i=0;while(i<lines.length-1&&n>lines[i].length){n-=lines[i++].length+1;}return new Position(i,n);},
 getText:range=>{if(!range)return text;let offset=p=>lines.slice(0,p.line).reduce((sum,l)=>sum+l.length+1,0)+p.character;return text.slice(offset(range.start),offset(range.end));},
 getWordRangeAtPosition:p=>{const line=lines[p.line],pattern=/[a-zA-Z_][a-zA-Z0-9_]*/g;for(const m of line.matchAll(pattern)){if(m.index<=p.character&&p.character<=m.index+m[0].length)return new Range(p.line,m.index,p.line,m.index+m[0].length);}return undefined;}};
 return doc;
}
function load(file,source){const m=new Module(file,module);m.filename=file;m.paths=Module._nodeModulePaths(path.dirname(file));const real=m.require.bind(m);m.require=id=>id==='vscode'?shim:real(id);m._compile(source??fs.readFileSync(file,'utf8'),file);return m.exports;}
function extension(prefix){const found=fs.readdirSync(EXT).filter(p=>p.startsWith(prefix+'-')).sort().pop();if(!found)throw Error('Missing extension '+prefix);return path.join(EXT,found);}
const results=[];
async function probe(provider,relative,needle,token,expected){
 const file=path.join(ROOT,relative),doc=document(file);const lines=doc.getText().split('\n');const row=lines.findIndex(l=>l.includes(needle));if(row<0)throw Error('Probe line missing: '+needle);
 const pos=new Position(row,lines[row].indexOf(token,lines[row].indexOf(needle))+1);
 const locations=await provider.provideDefinition(doc,pos,{isCancellationRequested:false})||[];
 const actual=locations.map(l=>l.uri.fsPath);const passed=actual.includes(path.join(ROOT,expected));
 results.push({source:relative,symbol:token,expected,actual,passed});console.log((passed?'PASS':'FAIL')+' '+relative+': '+token);
}
async function main(){
 const svFile=path.join(extension('eirikpre.systemverilog'),'dist/client/extension.js');
 let bundle=fs.readFileSync(svFile,'utf8');const start=bundle.lastIndexOf('var r={};(()=>{"use strict";var e=r;');if(start<0)throw Error('Unsupported SystemVerilog bundle layout');
 const req=load(svFile,bundle.slice(0,start)+'module.exports=n;})();');
 const {StringSource,SystemVerilogParser}=req(6997),parser=new SystemVerilogParser();
 const all=[];const targets=['vortex_StorageStacked/third_party/vortex/hw/rtl/VX_cluster.sv','vortex_StorageStacked/third_party/vortex/hw/rtl/VX_socket.sv','vortex_StorageStacked/third_party/vortex/hw/rtl/core/VX_core.sv','coralnpu_StorageStacked/coralnpu/hdl/verilog/rvv/design/rvv_backend_alu.sv','coralnpu_StorageStacked/coralnpu/hdl/verilog/rvv/design/rvv_backend_alu_unit.sv'];
 for(const relative of targets){const file=path.join(ROOT,relative);all.push(...parser.parse(new StringSource(fs.readFileSync(file,'utf8'),file),'fast',0));}
 const client={queryByName:async(name,options)=>all.filter(s=>s.name===name&&!options?.excludeTypes?.includes(s.type)),getFileSymbols:async(file,excluded)=>all.filter(s=>s.file===file&&!excluded?.includes(s.type)),getFileMeta:async()=>({})};
 const indexer={client,updateMostRecentSymbols:async()=>{}};
 workspaceProvider=new (req(2081).SystemVerilogWorkspaceSymbolProvider)(indexer);
 documentProvider=new (req(7669).SystemVerilogDocumentSymbolProvider)(indexer);
 const provider=new (req(2889).SystemVerilogDefinitionProvider)();

 await probe(provider,targets[0],'VX_socket #(','VX_socket',targets[1]);
 await probe(provider,targets[1],'VX_core #(','VX_core',targets[2]);
 await probe(provider,targets[3],'rvv_backend_alu_unit #(','rvv_backend_alu_unit',targets[4]);
 const tags=extension('jtanx.ctagsx');load(path.join(tags,'extension.js')).activate({subscriptions:[]});
 await probe(scalaProvider,'coralnpu_StorageStacked/coralnpu/hdl/chisel/src/coralnpu/Core.scala','new Parameters','Parameters','coralnpu_StorageStacked/coralnpu/hdl/chisel/src/coralnpu/Parameters.scala');
 await probe(scalaProvider,'coralnpu_StorageStacked/coralnpu/hdl/chisel/src/coralnpu/scalar/SCore.scala','new UncachedFetch(p, cacheL0','UncachedFetch','coralnpu_StorageStacked/coralnpu/hdl/chisel/src/coralnpu/scalar/UncachedFetch.scala');
 await probe(scalaProvider,'coralnpu_StorageStacked/coralnpu/hdl/chisel/src/coralnpu/scalar/UncachedFetch.scala','new FetchL0Cache(p)','FetchL0Cache','coralnpu_StorageStacked/coralnpu/hdl/chisel/src/coralnpu/scalar/FetchL0Cache.scala');
 fs.writeFileSync(path.join(__dirname,'hdl_navigation_verification.json'),JSON.stringify(results,null,2)+'\n');
 if(results.some(r=>!r.passed))process.exitCode=1;
}
main().catch(e=>{console.error(e);process.exitCode=1;});

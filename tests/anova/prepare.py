"""准备五软件共用的真实CSV和原生报告；不修改用户配置。"""
import csv, json, pathlib, subprocess, sys, hashlib

root=pathlib.Path(__file__).resolve().parents[2]
out=pathlib.Path(sys.argv[1]).resolve()
out.mkdir(parents=True,exist_ok=True)
cli=root/'build/Release/msa_cli.exe'
cases=[]
def invoke(args):
    completed=subprocess.run([str(cli),*map(str,args)],capture_output=True,encoding='utf-8')
    if completed.returncode: raise RuntimeError(completed.stderr)
def add(name,cfg,matrix=None):
    folder=out/name
    folder.mkdir()
    config=folder/'config.json'; data=folder/'data.csv'; report=folder/'native.json'
    config.write_text(json.dumps(cfg,ensure_ascii=False,indent=2),encoding='utf-8')
    if matrix is None:
        invoke(['--config',config,'--output',data,'--report',report])
    else:
        o,r,p=(cfg['design'][k] for k in ('operators','replicates','parts'))
        with data.open('w',encoding='utf-8',newline='') as stream:
            writer=csv.writer(stream)
            writer.writerow(['StdOrder','Operator','Part','Trial','Measurement'])
            for t in range(r):
                for j in range(p):
                    for i in range(o): writer.writerow([((t*p+j)*o+i+1),chr(65+i),j+1,t+1,matrix[i*r+t][j]])
        invoke(['--config',config,'--input',data,'--report',report])
    cases.append(dict(id=name,folder=str(folder),csv=str(data),native=str(report),config=str(config)))
for o in (2,3):
    for r in (2,3):
        for p in range(5,11):
            cfg=json.loads((root/'examples/示例配置.json').read_text(encoding='utf-8-sig'))
            cfg['design']=dict(operators=o,replicates=r,parts=p)
            cfg['analysis'].update(method='crossed_anova',interaction='auto',interaction_alpha=.05)
            cfg['generation'].update(seed=123+p,decimal_places=6)
            cfg['output'].update(format='minitab',run_order=dict(mode=['standard','all','within_operator'][p%3],seed=321,randomize_operators=True,store_standard_order=True))
            add(f'o{o}_r{r}_p{p}',cfg)
for name,interaction,op in [('pooled',0,1),('interaction',1,1),('negative_operator',1,0)]:
    cfg=json.loads((root/'examples/默认配置.json').read_text(encoding='utf-8-sig'))
    cfg['design']=dict(operators=2,replicates=2,parts=5)
    cfg['analysis'].update(method='crossed_anova',interaction='auto',interaction_alpha=.05)
    cfg['output'].update(format='minitab',run_order=dict(mode='standard',seed=321,randomize_operators=False,store_standard_order=True))
    cfg['specification'].update(lsl=0,usl=200)
    matrix=[[100+3*(j-2)+(op*(1 if i else -1))+interaction*(1 if i else -1)*(j-2)+(1 if t else -1) for j in range(5)] for i in range(2) for t in range(2)]
    add(name,cfg,matrix)
(out/'manifest.json').write_text(json.dumps(dict(cases=cases,generation_cli_sha256=hashlib.sha256(cli.read_bytes()).hexdigest()),indent=2),encoding='utf-8')
print(f'已准备{len(cases)}组共用CSV：{out}')

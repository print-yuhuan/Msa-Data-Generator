"""对比原生结果，保留软件原始估计；统一EMS负分量归零及SD/TV口径。"""
import csv,json,math,pathlib,sys,xml.etree.ElementTree as ET,hashlib
root=pathlib.Path(sys.argv[1]).resolve()
manifest=json.loads((root/'manifest.json').read_text(encoding='utf-8-sig'))
NS={'o':'http://www.ibm.com/software/analytics/spss/xml/oms'}
errors={}; checks=0; cases=[]
def load(path):return json.loads(path.read_text(encoding='utf-8-sig'))
def near(label,a,b):
    global checks
    if not math.isfinite(a) or not math.isfinite(b):raise AssertionError(f'{label}: 非有限数值')
    error=abs(a-b);errors[label]=max(errors.get(label,0),error)
    if error>1e-10+1e-8*abs(b):raise AssertionError(f'{label}: {a} != {b}, error={error}')
    checks+=1
def metrics(v):
    e,op,inter,p=[max(0,x) for x in v]
    variances=dict(EV=e,Operator=op,OperatorPart=inter,AV=op+inter,GRR=e+op+inter,PV=p,TV=e+op+inter+p)
    return variances
def from_table(full,pooled,o,r,p,included,origin=False):
    # Origin表另有Model行；MATLAB直接返回各来源及误差。
    selected=full if included else pooled
    mse=selected[4][2] if origin else selected[-1][2]
    msp,mso=selected[0][2],selected[1][2]
    msi=full[2][2] if included else mse
    return metrics([mse,(mso-msi)/(p*r),(msi-mse)/r if included else 0,(msp-msi)/(o*r)])
def numeric_csv(path):
    with path.open(encoding='utf-8-sig',newline='') as f:return list(csv.reader(f))
def check_import(label,actual,source):
    if len(actual)!=len(source):raise AssertionError(label+'记录数错误')
    for row,expected in zip(actual,source):
        if row!=expected:raise AssertionError(label+'导入对应关系或双精度值错误')
for item in manifest['cases']:
    folder=pathlib.Path(item['folder']);native=load(folder/'native.json');cfg=native['config'];an=native['metrics']['anova']
    o,r,p=[cfg['design'][k] for k in ('operators','replicates','parts')]
    included=an['interaction_included'];alpha=cfg['analysis']['interaction_alpha']
    with pathlib.Path(item['csv']).open(encoding='utf-8',newline='') as f:
        records=list(csv.DictReader(f))
    source=[[int(v['StdOrder']),ord(v['Operator'])-64,int(v['Part']),int(v['Trial']),float(v['Measurement'])] for v in records]
    assert sorted(v[0] for v in source)==list(range(1,len(source)+1))
    for row in source:assert row[0]==((row[3]-1)*p+row[2]-1)*o+row[1]
    external={}
    mtb=load(folder/'minitab.json');assert mtb['interaction_included']==included and mtb['import_exact']
    assert int(mtb['ndc'])==native['metrics']['ndc']
    external['Minitab']={key:float(mtb['stddev'].get(key,0))**2 for key in an['variance_components']}
    ml=load(folder/'matlab.json');check_import('MATLAB',ml['imported'],source)
    near('MATLAB.interaction_p',ml['interaction_p'],an['interaction_p'])
    assert (ml['interaction_p']<alpha)==included
    external['MATLAB']=from_table(ml['full'],ml['pooled'],o,r,p,included)
    orig=load(folder/'origin.json');check_import('Origin',orig['imported'],source)
    near('Origin.interaction_p',orig['interaction_p'],an['interaction_p'])
    assert (orig['interaction_p']<alpha)==included
    external['Origin']=from_table(orig['full'],orig['pooled'],o,r,p,included,True)
    spss=load(folder/'spss.json')
    check_import('SPSS',[[int(v[0]),ord(v[1].strip())-64,int(v[2]),int(v[3]),float(v[4])] for v in spss['imported']],source)
    sx=ET.parse(folder/'spss-full.xml')
    table=sx.find(".//o:pivotTable[@subType='Test of Between Subjects Mixed Effects']",NS)
    group=table.find(".//o:group[@text='Part * Operator']",NS)
    vals=[float(v.attrib['number']) for v in group.findall('.//o:cell',NS) if 'number' in v.attrib]
    near('SPSS.interaction_p',vals[4],an['interaction_p']);assert(vals[4]<alpha)==included
    selected=spss['full' if included else 'pooled']
    external['SPSS']=metrics([selected['Var(Error)'],selected['Var(Operator)'],selected.get('Var(Part * Operator)',0),selected['Var(Part)']])
    jt=numeric_csv(folder/'jmp-full-tests.csv')[1:]
    jp=float(jt[2][5]);near('JMP.interaction_p',jp,an['interaction_p']);assert(jp<alpha)==included
    jv=numeric_csv(folder/('jmp-'+('full' if included else 'pooled')+'-variance.csv'))[1:]
    estimates={row[0].split('&')[0]:float(row[1]) for row in jv}
    op_est=next(value for key,value in estimates.items() if key=='Operator')
    p_est=next(value for key,value in estimates.items() if key=='Part')
    inter_est=next((value for key,value in estimates.items() if '*' in key),0)
    residual=float(jv[-2][1])
    external['JMP']=metrics([residual,op_est,inter_est,p_est])
    final_jmp=numeric_csv(folder/('jmp-'+('full' if included else 'pooled')+'-tests.csv'))[1:]
    selected_xml=ET.parse(folder/('spss-'+('full' if included else 'pooled')+'.xml'))
    mixed=selected_xml.find(".//o:pivotTable[@subType='Test of Between Subjects Mixed Effects']",NS)
    for index,row in enumerate(an['table'][:3 if included else 2]):
        jrow=final_jmp[index]
        for label,actual,expected in [('ss',float(jrow[1]),row['ss']),('ms',float(jrow[2]),row['ms']),('f',float(jrow[4]),row['f']),('p',float(jrow[5]),row['p'])]:
            near('JMP.final.'+label,actual,expected)
        group=mixed.find(".//o:group[@text='"+row['source'].replace('*',' * ')+"']",NS)
        values=[float(v.attrib['number']) for v in group.findall('.//o:cell',NS) if 'number' in v.attrib]
        for label,actual,expected in [('ss',values[0],row['ss']),('ms',values[2],row['ms']),('f',values[3],row['f']),('p',values[4],row['p'])]:
            near('SPSS.final.'+label,actual,expected)
        assert int(values[1])==row['df'] and int(float(jrow[3]))==row['df']
    ji=numeric_csv(folder/'jmp-import.csv');columns={k:i for i,k in enumerate(ji[0])}
    check_import('JMP',[[int(float(row[columns['StdOrder']])),ord(row[columns['Operator']])-64,int(float(row[columns['Part']])),int(float(row[columns['Trial']])),float(row[columns['Measurement']])] for row in ji[1:]],source)
    for name,components in external.items():
        sd={k:math.sqrt(value) for k,value in components.items()}
        for k in components:
            near(name+'.variance.'+k,components[k],an['variance_components'][k])
            near(name+'.sd.'+k,sd[k],an['stddev'][k])
            near(name+'.study_variation.'+k,sd[k]*cfg['analysis']['study_multiplier'],an['study_variation'][k])
            near(name+'.contribution.'+k,100*components[k]/components['TV'],an['contribution_pct'][k])
            near(name+'.study_pct.'+k,100*sd[k]/sd['TV'],an['study_variation_pct'][k])
        near(name+'.PT_pct',100*sd['GRR']*cfg['analysis']['study_multiplier']/(cfg['specification']['usl']-cfg['specification']['lsl']),native['metrics']['PT_pct'])
        assert max(1,math.floor(1.41*sd['PV']/sd['GRR']))==native['metrics']['ndc']
    # 原生ANOVA表的SS/DF与两个独立计算引擎核对；不拿固定效应主效应F替代随机效应F。
    for name,data,indices in [('MATLAB',ml['full'],[0,1,2,3]),('Origin',orig['full'],[0,1,2,4])]:
        for idx,row in zip(indices,an['full_table'][:4]):
            near(name+'.full.ss',data[idx][0],row['ss']);assert int(data[idx][1])==row['df']
    cases.append(dict(id=item['id'],records=len(source),interaction_included=included,passed=True,csv_sha256=hashlib.sha256(pathlib.Path(item['csv']).read_bytes()).hexdigest()))
summary=dict(passed=True,cases=cases,case_count=len(cases),software_case_comparisons=5*len(cases),numeric_checks=checks,absolute_tolerance=1e-10,relative_tolerance=1e-8,maximum_absolute_errors=errors,scope='Minitab Gage R&R / JMP EMS / SPSS VARCOMP+UNIANOVA / MATLAB anovan / Origin worksheet+native ocMath API; EMS-derived GRR summaries for software without equivalent stored summaries')
(root/'comparison.json').write_text(json.dumps(summary,ensure_ascii=False,indent=2),encoding='utf-8')
print(json.dumps({k:v for k,v in summary.items() if k not in ('cases','maximum_absolute_errors')},ensure_ascii=False))

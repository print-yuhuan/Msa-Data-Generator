"""在已安装SPSS引擎中执行随机效应ANOVA和方差分量估计。"""
import json,pathlib,sys,xml.etree.ElementTree as ET
import spss
NS={'o':'http://www.ibm.com/software/analytics/spss/xml/oms'}
manifest=json.loads(pathlib.Path(sys.argv[1]).read_text(encoding='utf-8-sig'))
spss.Submit('SET OLANG=ENGLISH.\nSET PRINTBACK=OFF.')
for item in manifest['cases']:
    folder=pathlib.Path(item['folder'])
    source=pathlib.Path(item['csv']).as_posix()
    spss.Submit(f"GET DATA /TYPE=TXT /FILE='{source}' /ENCODING='UTF8' /DELCASE=LINE /DELIMITERS=',' /ARRANGEMENT=DELIMITED /FIRSTCASE=2 /VARIABLES=StdOrder F8.0 Operator A1 Part F8.0 Trial F8.0 Measurement F24.10.")
    cursor=spss.Cursor()
    imported=list(cursor.fetchall()); cursor.close()
    result={'version':spss.GetDefaultPlugInVersion(),'records':spss.GetCaseCount(),'imported':imported}
    for mode,design in [('full','Part Operator Part*Operator'),('pooled','Part Operator')]:
        xml=(folder/f'spss-{mode}.xml').as_posix()
        spss.Submit(f"OMS /SELECT TABLES /DESTINATION FORMAT=OXML OUTFILE='{xml}' VIEWER=NO.\nVARCOMP Measurement BY Part Operator /RANDOM=Part Operator /METHOD=SSTYPE(3) /PRINT=SS EMS /DESIGN={design}.\nUNIANOVA Measurement BY Part Operator /RANDOM=Part Operator /METHOD=SSTYPE(3) /DESIGN={design}.\nOMSEND.")
        tree=ET.parse(xml)
        table=tree.find(".//o:pivotTable[@subType='Variance Estimates']",NS)
        if table is None: raise RuntimeError(f'SPSS无方差估计表：{item["id"]}')
        values={}
        for row in table.findall('./o:dimension/o:category',NS):
            cell=row.find('.//o:cell',NS)
            if cell is not None and 'number' in cell.attrib: values[row.attrib['text']]=float(cell.attrib['number'])
        result[mode]=values
        result[mode+'_anova']=[]
        table=tree.find(".//o:pivotTable[@subType='ANOVA']",NS)
        for row in table.findall('./o:dimension/o:category',NS):
            cells=[float(cell.attrib['number']) for cell in row.findall('.//o:cell',NS) if 'number' in cell.attrib]
            result[mode+'_anova'].append(dict(source=row.attrib['text'],values=cells))
    (folder/'spss.json').write_text(json.dumps(result),encoding='utf-8')
    print('SPSS完成 '+item['id'],flush=True)
spss.StopSPSS()

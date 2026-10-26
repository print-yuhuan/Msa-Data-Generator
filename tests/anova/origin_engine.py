"""通过Origin公开ocMath接口核对原生ANOVA；输入是Origin工作表回读数据。"""
import ctypes as C, json,os,pathlib,sys

class Description(C.Structure):
    _fields_=[('N',C.c_int),('Mean',C.c_double),('SD',C.c_double),('SEM',C.c_double),('VAR',C.c_double),('Missing',C.c_int),('NonMissing',C.c_int)]
class Row(C.Structure):
    _fields_=[('df',C.c_int),('ss',C.c_double),('ms',C.c_double),('f',C.c_double),('p',C.c_double)]
import PyOrigin
manifest_path=PyOrigin.LT_get_str('msa_manifest')
program=pathlib.Path('C:/Program Files/OriginLab/Origin2026b')
directory=os.add_dll_directory(str(program))
engine=C.CDLL(str(program/'ocMath.dll'))
function=engine.ocmath_stats_anova_two_way_compute_anova_table
function.restype=C.c_int
function.argtypes=[C.c_int,C.POINTER(C.c_int),C.c_int,C.POINTER(C.c_int),C.c_int,C.POINTER(C.c_double),C.c_int,C.POINTER(Description),C.POINTER(Description),C.POINTER(Description),C.POINTER(Description),C.POINTER(Row),C.c_void_p,C.c_void_p]
manifest=json.loads(pathlib.Path(manifest_path).read_text(encoding='utf-8-sig'))
for item in manifest['cases']:
    folder=pathlib.Path(item['folder'])
    imported=json.loads((folder/'origin-import.json').read_text(encoding='utf-8-sig'))
    values=imported['data']; n=len(values)
    p=max(int(row[2]) for row in values); o=max(int(row[1]) for row in values)
    a=(C.c_int*n)(*[int(row[2]) for row in values]);b=(C.c_int*n)(*[int(row[1]) for row in values]);y=(C.c_double*n)(*[row[4] for row in values])
    result={'interface':'Origin ocMath native ANOVA API; Origin COM worksheet round-trip','imported':values,'records':n}
    for label,interaction in [('full',1),('pooled',0)]:
        # 描述统计不参与对比，作为不透明输出预留缓冲；仅解释公开ANOVA表。
        # 此适配器限定本机10.3.5接口，检查尾部保护区，避免依赖未使用结构的布局。
        buffers=[C.create_string_buffer(65536) for _ in range(4)]
        for buffer in buffers:C.memset(C.addressof(buffer)+65024,0xA5,512)
        da,db,dt,dab=[C.cast(buffer,C.POINTER(Description)) for buffer in buffers]
        table=(Row*6)()
        fitted=(C.c_double*n)();residuals=(C.c_double*n)()
        rc=function(interaction,a,p,b,o,y,n,da,db,dt,dab,table,C.cast(fitted,C.c_void_p),C.cast(residuals,C.c_void_p))
        if any(buffer.raw[65024:]!=b'\xa5'*512 for buffer in buffers):raise RuntimeError('Origin描述统计缓冲区越界')
        if rc!=0:raise RuntimeError(f'Origin ANOVA error {rc}: {item["id"]}')
        result[label]=[[row.ss,row.df,row.ms] for row in table]
        if interaction:result['interaction_p']=table[2].p
    (folder/'origin.json').write_text(json.dumps(result),encoding='utf-8')
PyOrigin.LT_set_var('msa_python_ok',1)

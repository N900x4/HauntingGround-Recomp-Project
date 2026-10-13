import sys, re
sys.argv=['m2parse.py','/CAPCOM.SFD','0']
src=open('m2parse.py').read()
exec(src.replace("parse(v, int(sys.argv[2]) if len(sys.argv) > 2 else 1000)",""))
def run(variant, maxpic=12):
    import io, contextlib
    T2=dict(T)
    if variant=='swapB10':
        t=dict(T['t_mbt_b']); t['10'],t['11']=T['t_mbt_b']['11'],T['t_mbt_b']['10']; T2['t_mbt_b']=t
    g=globals(); old=g['T']; g['T']=T2
    global slice_
    f=io.StringIO()
    with contextlib.redirect_stdout(f): parse(v, maxpic)
    g['T']=old
    return f.getvalue().strip().splitlines()[-1]
print('base   ', run('base'))
print('swapB10', run('swapB10'))

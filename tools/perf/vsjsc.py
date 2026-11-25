import sys,math,collections
d=collections.defaultdict(lambda:collections.defaultdict(list))
for l in open(sys.argv[1]):
    p=l.split()
    if len(p)==5: d[p[1]][p[2]].append((float(p[3]),float(p[4])))
rows=[]
for n,m in d.items():
    L=sys.argv[2] if len(sys.argv)>2 else 'l'
    if 'j' in m and L in m:
        jf,jl=min(x[0] for x in m['j']),min(x[1] for x in m['j']); lf,ll=min(x[0] for x in m[L]),min(x[1] for x in m[L])
        rows.append((ll/jl,lf/jf,n,ll,jl,lf,jf))
rows.sort(reverse=True)
g=lambda v:math.exp(sum(map(math.log,v))/len(v))
for r in rows: print('%-32s later %6.2fx (%8.2f vs %8.2f)  first %5.2fx (%7.1f vs %7.1f)'%(r[2],r[0],r[3],r[4],r[1],r[5],r[6]))
print(len(rows),'geomean later %.3f first %.3f'%(g([r[0] for r in rows]),g([r[1] for r in rows])))

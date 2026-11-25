import sys,collections,math
d=collections.defaultdict(lambda:collections.defaultdict(list))
for l in open(sys.argv[1]):
    n,m,f,x=l.split(); d[n][m].append((float(x),float(f)))
g=[];gf=[];rows=[]
for n,v in d.items():
    if not v["A"] or not v["B"]: print(n,"missing"); continue
    a=min(v["A"])[0];b=min(v["B"])[0]; fa=min(x[1] for x in v["A"]); fb=min(x[1] for x in v["B"])
    g.append(b/a); gf.append(fb/fa); rows.append((b/a,n,a,b,fa,fb))
for r in sorted(rows): print("%-30s later %8.3f -> %8.3f %+6.2f%%   first %7.1f -> %7.1f"%(r[1],r[2],r[3],100*(r[0]-1),r[4],r[5]))
G=lambda v:100*(math.exp(sum(map(math.log,v))/len(v))-1)
print("geomean later %+.2f%% first %+.2f%% over %d"%(G(g),G(gf),len(g)))

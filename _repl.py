import io, sys
path=sys.argv[1]
with io.open("_old.txt","r",encoding="utf-8") as f: old=f.read()
with io.open("_new.txt","r",encoding="utf-8") as f: new=f.read()
with io.open(path,"r",encoding="utf-8") as f: data=f.read()
n=data.count(old)
if n!=1:
    print("ERR count",n); sys.exit(1)
out=data.replace(old,new); ob=out.encode("utf-8"); ol=len(data.encode("utf-8"))
with io.open(path,"r+b") as f:
    f.write(ob + b" "*(ol-len(ob)) if len(ob)<ol else ob)
print("OK",ol,"->",len(ob))

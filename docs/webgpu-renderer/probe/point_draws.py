import struct,collections,os,sys
HDR=struct.Struct('<4sIIIIQQ'); CTX_N=0x400; FET_N=0x200; REC=HDR.size+24+CTX_N*4+FET_N*4
d=open(sys.argv[1],'rb').read(); SH=sys.argv[2]
c=collections.Counter(); hashes=set(); missing=set(); rect=collections.Counter()
for i in range(len(d)//REC):
    r=d[i*REC:(i+1)*REC]; kind,win,prim,count,major,vh,ph=HDR.unpack_from(r)
    if kind!=b'DRAW': continue
    ctx=struct.unpack_from('<%dI'%CTX_N,r,HDR.size+24)
    R=lambda x: ctx[x-0x2000]
    for h,k in ((vh,'vert'),(ph,'frag')):
        if h:
            hashes.add((h,k))
            if not os.path.exists(os.path.join(SH,'shader_%016X.ucode.%s'%(h,k))): missing.add((h,k))
    if prim in (1,8,0xD,5):
        vsme='me' if os.path.exists(os.path.join(SH,'shader_%016X.ucode.vert'%vh)) and 'eA' in open(os.path.join(SH,'shader_%016X.ucode.vert'%vh)).read() else ''
        ps_size=R(0x2280); 
        key=(prim,'edram%d'%(R(0x2208)&7),vsme,'ps' if ph else 'nops','ptsize=%08x'%ps_size,'count=%d'%count if prim!=1 else 'cnt>%d'%(count//64*64), 'vs=%016X'%vh)
        c[key]+=1
print('unique shaders seen',len(hashes),'(vs',sum(1 for h in hashes if h[1]=='vert'),') missing from dump',len(missing))
for k,v in c.most_common(30): print(v,k)

# Rewrite only Clarifae OUTPUT applies in a pinned, audited XML template.
function fail(code) { bad=code;exit code }
BEGIN {
 n=split(targets,a,",");
 for(i=1;i<=n;i++)if(a[i]!="") {
  if(a[i]!~/^(music|voice_call|ring|alarm|notification|system)$/ || want[a[i]])fail(65)
  want[a[i]]=1
 } else if(targets!="")fail(65)
 post=0;blocks=0;cur=""
}
/<postprocess>/ {post=1;blocks++}
{
 if(post && /<stream[ \t]/) {
  if(cur!="")fail(61)
  if($0!~/^[ \t]*<stream[ \t]+type="[A-Za-z0-9_]+"[ \t]*>[ \t]*$/)fail(62)
  if(!match($0,/type="[^"]+"/))fail(62)
  cur=substr($0,RSTART+6,RLENGTH-7)
  if(seen[cur]++)fail(67)
 }
 if(post && /<apply.*clarifae_out/) {
  if(cur=="" || $0!~/^[ \t]*<apply[ \t]+effect="clarifae_out"[ \t]*\/>[ \t]*$/)fail(66)
  next
 }
 if(post && /<\/stream>/) {
  if(cur=="" || $0!~/^[ \t]*<\/stream>[ \t]*$/)fail(63)
  if(want[cur]){print "            <apply effect=\"clarifae_out\"/>";done[cur]=1}
  cur=""
 }
 if(/<\/postprocess>/) {
  for(i=1;i<=n;i++)if(a[i]!="" && !done[a[i]]) {
   print "        <stream type=\"" a[i] "\">"
   print "            <apply effect=\"clarifae_out\"/>"
   print "        </stream>";done[a[i]]=1
  }
  post=0
 }
 print
}
END {if(bad)exit bad;if(blocks!=1 || post || cur!="")exit 64}

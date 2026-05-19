#!/usr/bin/env bash
# results.csv -> report.md. Median + [min-max] dispersion per cell,
# keyed on (workload, table_size, threads). Pure awk+sort.
# CSV cols: sut,workload,table_size,threads,rep,tps,qps,p95_ms,cpu_sec,mem_peak_mb,datadir_bytes
set -uo pipefail
CSV="${1:?usage: gen-report.sh <results.csv>}"
OUT="$(dirname "$CSV")/report.md"

{
  echo "# Performance report"
  echo
  echo "A = MySQL 9.7 + tidesdb-mysql v0.2.1 · B = MariaDB 12.3.1 + upstream tidesql."
  echo "Shared TidesDB v9.2.0; identical caps + benchmarking server config (README §3)."
  echo "Cells: **median** \`[min–max]\` over reps. Higher TPS better; lower p95/CPU better."
  echo "\`table_size\` is the contention axis (smaller = higher write conflict for OCC)."
  echo
  echo "| Workload | tbl_size | Thr | TPS A | TPS B | A/B | p95ms A | p95ms B | cpu_s A | cpu_s B |"
  echo "|---|---|---|---|---|---|---|---|---|---|"
  awk -F, 'NR>1{
      key=$2 SUBSEP $3 SUBSEP $4 SUBSEP $1
      tps[key]=tps[key] $6 " "; p95[key]=p95[key] $8 " "; cpu[key]=cpu[key] $9 " "
      cell[$2 SUBSEP $3 SUBSEP $4]=1
  }
  function num(x){ return (x=="NA"||x=="")?0:x }
  function agg(s,  a,n,i,c,t,lo,hi,md){ n=split(s,a," ");
      if(n==0) return "NA"
      for(i=1;i<=n;i++) a[i]=num(a[i])
      for(i=1;i<=n;i++)for(c=i+1;c<=n;c++) if(a[c]+0<a[i]+0){t=a[i];a[i]=a[c];a[c]=t}
      lo=a[1]; hi=a[n]
      md=(n%2)? a[(n+1)/2] : (a[n/2]+a[n/2+1])/2
      return sprintf("%.2f [%.2f-%.2f]", md, lo, hi) }
  function medonly(s,  a,n,i,c,t){ n=split(s,a," "); if(n==0)return 0
      for(i=1;i<=n;i++)a[i]=num(a[i])
      for(i=1;i<=n;i++)for(c=i+1;c<=n;c++)if(a[c]+0<a[i]+0){t=a[i];a[i]=a[c];a[c]=t}
      return (n%2)?a[(n+1)/2]:(a[n/2]+a[n/2+1])/2 }
  END{ for(x in cell){ split(x,p,SUBSEP); w=p[1]; tz=p[2]; th=p[3]
        ka=w SUBSEP tz SUBSEP th SUBSEP "mysql"
        kb=w SUBSEP tz SUBSEP th SUBSEP "mariadb"
        ma=medonly(tps[ka]); mb=medonly(tps[kb])
        r=(mb>0)?sprintf("%.2f",ma/mb):"NA"
        printf "%s\t%012d\t%03d\t| %s | %s | %s | %s | %s | %s | %s | %s | %s | %s |\n",
          w, tz, th, w, tz, th, agg(tps[ka]), agg(tps[kb]), r,
          agg(p95[ka]), agg(p95[kb]), agg(cpu[ka]), agg(cpu[kb]) }
  }' "$CSV" | sort | cut -f4-
} >"$OUT"
echo "[report] -> $OUT"
cat "$OUT"

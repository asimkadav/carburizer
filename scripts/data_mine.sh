echo '=====================Infinite Loops=================================';
awk '$1=="ticks" {printf "%s %s\n",$0,p;} {p=$0;}' make_log.txt >   \
single_line.txt
awk '{if (NF == 19) {split($19,a,"/");printf "%d %s\n",$2,a[2];}} {if (NF == 20) \
{split ($20,b,"/");printf "%d %s\n",$2,b[2];}} ' single_line.txt   > ticks_dir.txt
awk '{if (NF == 19) {split($19,a,"/");printf "%d %s\n",$4,a[2];}} {if (NF == 20) \
{split ($20,b,"/");printf "%d %s\n",$4,b[2];}} ' single_line.txt   > \
array_dir.txt
awk ' {a[$2] +=$1;a[total] += $1;} END{ for (dir in a) printf "%s, %d\n",dir,a[dir];printf "total %d\n",a[total];}' ticks_dir.txt  
echo '====================Array unsafe=============================================';
awk ' {a[$2] +=$1;a[total] += $1;} END{ for (dir in a) printf "%s, %d\n",dir,a[dir];printf "total %d\n",a[total];}' array_dir.txt
echo '====================Mem De-ref===========================================';
./deref.sh
echo '=====================Return on halt============================================';
awk '{if (NF == 19) {split($19,a,"/");printf "%d %s\n",$9,a[2];}} {if (NF == 20) {split ($20,b,"/");printf "%d %s\n",$9,b[2];}} ' single_line.txt  > md.txt
awk ' {a[$2] +=$1;a[total] += $1;} END{ for (dir in a) printf "%s, %d\n",dir,a[dir]; printf "total %d\n",a[total];}' md.txt
echo '=====================Report on ret==========================================';
awk '{if (NF == 19) {split($19,a,"/");printf "%d %s\n",$11,a[2];}} {if (NF == 20) {split ($20,b,"/");printf "%d %s\n",$11,b[2];}} ' single_line.txt  > md.txt
awk ' {a[$2] +=$1;a[total] += $1;} END{ for (dir in a) printf "%s, %d\n",dir,a[dir]; printf "total %d\n",a[total];}' md.txt
echo '=====================Report on false stuck-at==========================================';
awk '{if (NF == 19) {split($19,a,"/");printf "%d %s\n",$11,a[2];}} {if (NF == 20) {split ($20,b,"/");printf "%d %s\n",$13,b[2];}} ' single_line.txt  > md.txt
awk ' {a[$2] +=$1;a[total] += $1;} END{ for (dir in a) printf "%s, %d\n",dir,a[dir]; printf "total %d\n",a[total];}' md.txt
echo '=====================Security Bugs==========================================';
awk '{if (NF == 19) {split($19,a,"/");printf "%d %s\n",$13,a[2];}} {if (NF == 20) {split ($20,b,"/");printf "%d %s\n",$15,b[2];}} ' single_line.txt  > md.txt
awk ' {a[$2] +=$1;a[total] += $1;} END{ for (dir in a) printf "%s, %d\n",dir,a[dir]; printf "total %d\n",a[total];}' md.txt
echo '=====================schedule() in infinite loop==========================================';
awk '{if (NF == 19) {split($19,a,"/");printf "%d %s\n",$15,a[2];}} {if (NF == 20) {split ($20,b,"/");printf "%d %s\n",$17,b[2];}} ' single_line.txt  > md.txt
awk ' {a[$2] +=$1;a[total] += $1;} END{ for (dir in a) printf "%s, %d\n",dir,a[dir]; printf "total %d\n",a[total];}' md.txt

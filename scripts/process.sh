 cat single_line.txt | awk ' {if($2 > 0) print $0 > "bugs_32.txt"; }';


find  -name  "*.i" -exec rm {} \; 
find  -name  "*.cil.*" -exec rm {} \; 
find ./drivers/ -name "*.ko" -exec rm {} \;
find ./drivers/ -name "*.o" -exec rm {} \;
find ./sound/ -name "*.ko" -exec rm {} \;
find ./sound/ -name "*.o" -exec rm {} \;


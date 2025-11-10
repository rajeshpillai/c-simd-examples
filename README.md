# Generate large CSV
./make_big_csv.sh 1000000

# Compile SIMD
gcc -o3 -mavx2 simd_csv.c -o simd_csv

## Run the code
./simd_csv bigfile.csv

# Compile Non SIMD code 
gcc -o3 -std-c11 nosimd_csv.c -o nosimd_csv 


# Run

1) Count commas (non-SIMD baseline):

./nosimd_csv bigfile.csv --count-commas


2) Parse CSV and print stats:

./nosimd_csv bigfile.csv


3) Parse and print the first 5 rows (as TSV for readability):

./nosimd_csv bigfile.csv --print --limit 5

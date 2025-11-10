#!/bin/bash
OUT="bigfile.csv"
ROWS=${1:-1000000}  # default: 1 million rows

echo "EmployeeID,Name,Dept,DaysWorked,Basic,Bonus,Tax" > "$OUT"

for ((i=1; i<=ROWS; i++)); do
  id=$(printf "E%06d" $i)
  name="Emp$i"
  dept=$(( (RANDOM % 10) + 1 ))
  days=$(( (RANDOM % 31) + 1 ))
  basic=$(( 20000 + (RANDOM % 15000) ))
  bonus=$(( (RANDOM % 5000) ))
  tax=$(( (basic + bonus) * 5 / 100 ))
  echo "$id,$name,Dept$dept,$days,$basic,$bonus,$tax" >> "$OUT"
done

echo "Created $OUT with $ROWS rows (~$(du -h $OUT | cut -f1))"


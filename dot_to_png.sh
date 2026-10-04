set -euo pipefail
IFS=$'\n\t'

DIR="${1:-.}"
if [ ! -d "$DIR" ]; then
  echo "Error: directory '$DIR' does not exist." >&2
  exit 2
fi

shopt -s nullglob
files=("$DIR"/*.dot)
if [ ${#files[@]} -eq 0 ]; then
  echo "No .dot files found in '$DIR'."
  exit 0
fi

count=0
fail=0
for f in "${files[@]}"; do
  outp="${f%.dot}.png"
  echo "Converting: $f -> $outp"
  if dot -Tpng "$f" -o "$outp"; then
    echo "  ✓ Success"
    count=$((count+1))
  else
    echo "  ✗ Failed" >&2
    fail=$((fail+1))
  fi
done

echo "Converted $count .dot file(s) to .png"
if [ $fail -ne 0 ]; then
  echo "$fail file(s) failed to convert" >&2
  exit 1
fi
exit 0

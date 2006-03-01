source common.sh

clearStore () {
    echo "clearing store..."
    chmod -R +w "$NIX_STORE_DIR"
    rm -rf "$NIX_STORE_DIR"
    mkdir "$NIX_STORE_DIR"
    rm -rf "$NIX_DB_DIR"
    mkdir "$NIX_DB_DIR"
    $nixstore --init
}

pullCache () {
    echo "pulling cache..."
    $PERL -w -I$TOP/scripts $TOP/scripts/nix-pull file://$TEST_ROOT/manifest
}

clearStore
pullCache

drvPath=$($nixinstantiate dependencies.nix)
outPath=$($nixstore -q $drvPath)

echo "building $outPath using substitutes..."
$nixstore -r $outPath

cat $outPath/input-2/bar

clearStore
pullCache

echo "building $drvPath using substitutes..."
$nixstore -r $drvPath

cat $outPath/input-2/bar

# Check that the derivers are set properly.
test $($nixstore -q --deriver "$outPath") = "$drvPath"
$nixstore -q --deriver $(/bin/ls -l $outPath/input-2 | sed 's/.*->\ //') | grep -q -- "-input-2.drv" 

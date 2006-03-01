source common.sh

rm -f $NIX_STATE_DIR/var/nix/
$nixstore --gc

# Produce an escaped log file.
$nixstore --log-type escapes -r -vv $($nixinstantiate dependencies.nix) 2> $TEST_ROOT/log.esc

# Convert it to an XML representation.
$TOP/src/nix-log2xml/nix-log2xml < $TEST_ROOT/log.esc > $TEST_ROOT/log.xml

# Is this well-formed XML?
if test -n "$xmllint"; then
    $xmllint --noout $TEST_ROOT/log.xml
fi

# Convert to HTML.
if test -n "$xsltproc"; then
    (cd $TOP/src/nix-log2xml && $xsltproc mark-errors.xsl - | $xsltproc log2html.xsl -) < $TEST_ROOT/log.xml > $TEST_ROOT/log.html
    # Ideally we would check that the generated HTML is valid...

    # A few checks...
    grep "<li>.*<code>.*echo FOO" $TEST_ROOT/log.html
fi

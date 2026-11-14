# hwdata pnp.ids -> vendor name lookup
BEGIN {
	print "#include \"platform/drm/connector/edid.h\"\n"
	print "#define PNP_ID(a, b, c) ((a & 0x1f) << 10) | ((b & 0x1f) << 5) | (c & 0x1f)"
	print "const char *drm_pnp_manufacturer(const char code[static 3]) {"
	print "\tswitch (PNP_ID(code[0], code[1], code[2])) {"
}
length($1) == 3 {
	vendor = $0
	sub(/^[^ \t]+[ \t]+/, "", vendor)
	printf "\tcase PNP_ID('%s', '%s', '%s'): return \"%s\";\n", \
		substr($1, 1, 1), substr($1, 2, 1), substr($1, 3, 1), vendor
}
END {
	print "\t}\n\treturn NULL;\n}\n#undef PNP_ID"
}

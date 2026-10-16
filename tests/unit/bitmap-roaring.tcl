# Tests of the Roaring encoding of bitmaps (bitmap-roaring-enabled). Every
# command must behave exactly as with the plain encoding, so most tests run the
# same commands on a plain key and on a Roaring-encoded key and compare.

# Runs a command on the plain key and on the Roaring key (names are given
# with the placeholder KEY), and checks that the replies are the same.
proc roaring_twin {args} {
    r config set bitmap-roaring-enabled no
    set plain [catch {r {*}[string map {KEY plain} $args]} pres]
    r config set bitmap-roaring-enabled yes
    set roar [catch {r {*}[string map {KEY roar} $args]} rres]
    assert_equal [list $plain $pres] [list $roar $rres]
    return $rres
}

proc roaring_assert_same_value {} {
    assert_equal [r get plain] [r get roar]
    assert_equal [r strlen plain] [r strlen roar]
    assert_equal [debug_digest_value plain] [debug_digest_value roar]
}

start_server {tags {"bitops roaring"}} {
    test {Roaring: disabled by default} {
        if {[lsearch $::global_overrides bitmap-roaring-enabled] == -1} {
            assert_equal {bitmap-roaring-enabled no} [r config get bitmap-roaring-enabled]
        }
        r config set bitmap-roaring-enabled no
        r del k
        r setbit k 100 1
        assert_encoding raw k
    }

    test {Roaring: SETBIT creates a Roaring-encoded string} {
        r config set bitmap-roaring-enabled yes
        r del k
        assert_equal 0 [r setbit k 100 1]
        assert_encoding roaring k
        assert_equal string [r type k]
        assert_equal 13 [r strlen k]
        assert_equal 1 [r getbit k 100]
        assert_equal 0 [r getbit k 101]
        assert_equal 0 [r getbit k 1000000]
        assert_equal 1 [r setbit k 100 0]
        assert_equal 0 [r getbit k 100]
        # Clearing every bit keeps the length, like the plain encoding.
        assert_equal 13 [r strlen k]
        assert_equal [string repeat "\x00" 13] [r get k]
    }

    test {Roaring: SETBIT converts a plain string} {
        r config set bitmap-roaring-enabled no
        r set k "foobar"
        r config set bitmap-roaring-enabled yes
        assert_encoding embstr k
        r setbit k 7 1
        assert_encoding roaring k
        assert_equal "goobar" [r get k]
        r set k 12345
        r setbit k 100 0
        assert_encoding roaring k
        assert_equal "12345[string repeat "\x00" 8]" [r get k]
    }

    test {Roaring: an existing Roaring string keeps its encoding when disabled} {
        r config set bitmap-roaring-enabled yes
        r del k
        r setbit k 10 1
        r config set bitmap-roaring-enabled no
        r setbit k 20 1
        assert_encoding roaring k
        r config set bitmap-roaring-enabled yes
    }

    test {Roaring: string commands see the same bytes} {
        r del plain roar
        foreach bit {0 1 7 8 9 100 65535 65536 65537 200000 1000000} {
            roaring_twin setbit KEY $bit 1
        }
        assert_encoding roaring roar
        roaring_assert_same_value
        foreach {start end} {0 0 0 -1 -1 -1 5 3 -5 -2 1000 200000 8191 8193 -200000 -1 0 1000000} {
            roaring_twin getrange KEY $start $end
        }
        roaring_twin lcs KEY plain
        roaring_twin sort KEY
    }

    test {Roaring: APPEND and SETRANGE keep the Roaring encoding} {
        r del plain roar
        roaring_twin setbit KEY 1000 1
        roaring_twin append KEY "abc"
        assert_encoding roaring roar
        roaring_assert_same_value

        r del plain roar
        roaring_twin setbit KEY 1000 1
        roaring_twin setrange KEY 3 "xyz"
        assert_encoding roaring roar
        roaring_assert_same_value
        roaring_twin setrange KEY 500 "past the end"
        roaring_twin setrange KEY 10 ""
        roaring_twin append KEY ""
        assert_encoding roaring roar
        roaring_assert_same_value

        r del plain roar
        roaring_twin setbit KEY 1000 1
        roaring_twin incr KEY
        assert_encoding roaring roar
    }

    test {Roaring: large SETRANGE and APPEND} {
        r del plain roar
        set rand [expr {srand(4321)}]
        for {set i 0} {$i < 3000} {incr i} {
            roaring_twin setbit KEY [expr {int(rand() * 3000000)}] 1
        }
        # Writes of all sizes, inside and across chunks of 8192 bytes, of
        # zeros, ones and random bytes, some extending the string. One of them
        # covers more than 128 chunks.
        foreach {offset len} {0 1 5 64 5 65 8000 400 8191 2 8190 8200 16384 8192                               100 50000 200000 300000 370000 30000 1200000 1100000} {
            foreach kind {ones zeros random} {
                switch $kind {
                    ones {set value [string repeat "\xff" $len]}
                    zeros {set value [string repeat "\x00" $len]}
                    random {
                        set value {}
                        for {set j 0} {$j < $len} {incr j} {
                            append value [format %c [expr {int(rand() * 256)}]]
                        }
                    }
                }
                roaring_twin setrange KEY $offset $value
                assert_encoding roaring roar
                roaring_assert_same_value
                roaring_twin bitcount KEY
            }
        }
        foreach len {1 63 64 65 9000 300000} {
            roaring_twin append KEY [string repeat "\x0f" $len]
            assert_encoding roaring roar
            roaring_assert_same_value
        }
    }

    test {Roaring: SETRANGE on a shared Roaring string} {
        r del plain roar copy
        r config set bitmap-roaring-enabled yes
        r setbit roar 100 1
        r copy roar copy
        r setrange roar 0 "\xff"
        assert_encoding roaring roar
        assert_equal 1 [r getbit roar 0]
        assert_equal 0 [r getbit copy 0]
        assert_equal 1 [r getbit copy 100]
    }

    test {Roaring: HyperLogLog commands see the same bytes} {
        r del plain roar
        roaring_twin pfadd KEY a b c d e
        roaring_twin setbit KEY 7 1
        assert_encoding roaring roar
        roaring_twin pfcount KEY
        roaring_twin pfadd KEY f g
        roaring_twin pfcount KEY
        roaring_assert_same_value
    }

    test {Roaring: BITCOUNT and BITPOS with ranges} {
        r del plain roar
        set rand [expr {srand(1234)}]
        for {set i 0} {$i < 2000} {incr i} {
            roaring_twin setbit KEY [expr {int(rand() * 300000)}] 1
        }
        # A run of ones covering more than a whole chunk of 65536 bits.
        r config set bitmap-roaring-enabled no
        r setrange plain 50000 [string repeat "\xff" 8750]
        r config set bitmap-roaring-enabled yes
        r del roar
        r bitop or roar plain
        assert_encoding roaring roar
        roaring_assert_same_value
        set len [r strlen plain]
        foreach {start end} [list 0 -1 0 0 -1 -1 5 3 100 [expr {$len - 1}] -9000 -100 \
                             49990 50010 40000 60000] {
            roaring_twin bitcount KEY $start $end
            roaring_twin bitcount KEY $start $end byte
            roaring_twin bitcount KEY [expr {$start * 8 + 3}] [expr {$end * 8 + 5}] bit
            foreach bit {0 1} {
                roaring_twin bitpos KEY $bit $start
                roaring_twin bitpos KEY $bit $start $end
                roaring_twin bitpos KEY $bit [expr {$start * 8 + 3}] [expr {$end * 8 + 5}] bit
            }
        }
        roaring_twin bitcount KEY
        roaring_twin bitpos KEY 0
        roaring_twin bitpos KEY 1
        # Inside the run of ones.
        roaring_twin bitpos KEY 0 [expr {400000 / 8 + 10}]
        roaring_twin bitpos KEY 0 [expr {400000 / 8 + 10}] [expr {400000 / 8 + 20}]
    }

    test {Roaring: BITPOS 0 on a string of all ones} {
        r del plain roar
        r config set bitmap-roaring-enabled no
        r set plain [string repeat "\xff" 10000]
        r config set bitmap-roaring-enabled yes
        r bitop or roar plain
        assert_encoding roaring roar
        roaring_twin bitpos KEY 0
        roaring_twin bitpos KEY 0 0 -1
        roaring_twin bitpos KEY 0 100
        roaring_twin bitpos KEY 0 7 79999 bit
        roaring_twin bitpos KEY 1 9999
    }

    test {Roaring: BITOP} {
        foreach op {and or xor not} {
            r del pa pb pc ra rb rc
            for {set i 0} {$i < 300} {incr i} {
                foreach {p q} {pa ra pb rb pc rc} {
                    set bit [expr {int(rand() * 100000)}]
                    r config set bitmap-roaring-enabled no
                    r setbit $p $bit 1
                    r config set bitmap-roaring-enabled yes
                    r setbit $q $bit 1
                }
            }
            r config set bitmap-roaring-enabled no
            if {$op eq {not}} {
                set plen [r bitop $op pdest pa]
            } else {
                set plen [r bitop $op pdest pa pb pc nokey]
            }
            r config set bitmap-roaring-enabled yes
            if {$op eq {not}} {
                set rlen [r bitop $op rdest ra]
            } else {
                set rlen [r bitop $op rdest ra rb rc nokey]
            }
            assert_equal $plen $rlen
            assert_encoding roaring rdest
            assert_equal [r get pdest] [r get rdest]
            assert_equal [r bitcount pdest] [r bitcount rdest]
        }
        # Mixed encodings as inputs, and the destination as an input.
        r config set bitmap-roaring-enabled yes
        r bitop and ra ra pb
        r config set bitmap-roaring-enabled no
        r bitop and pa pa pb
        assert_equal [r get pa] [r get ra]
        # Only missing keys: the destination is deleted.
        r config set bitmap-roaring-enabled yes
        assert_equal 0 [r bitop or rdest nokey1 nokey2]
        assert_equal 0 [r exists rdest]
    }

    test {Roaring: BITFIELD} {
        r del plain roar
        foreach {type offset value} {u8 0 255 i16 5 -1000 u63 100 123456789 i64 65530 -1 u1 65535 1 i7 131071 -64} {
            roaring_twin bitfield KEY set $type $offset $value
            roaring_twin bitfield KEY get $type $offset
            roaring_twin bitfield KEY incrby $type $offset 7
            roaring_twin bitfield KEY overflow sat incrby $type $offset 1000000000
            roaring_twin bitfield KEY overflow fail incrby $type $offset -1000000000
            roaring_twin bitfield_ro KEY get $type [expr {$offset + 3}]
        }
        assert_encoding roaring roar
        roaring_assert_same_value
    }

    test {Roaring: COPY makes an independent copy} {
        r config set bitmap-roaring-enabled yes
        r del k k2
        r setbit k 5 1
        r copy k k2
        assert_encoding roaring k2
        r setbit k2 6 1
        assert_equal 0 [r getbit k 6]
        assert_equal 1 [r getbit k2 6]
    }

    test {Roaring: DUMP and RESTORE} {
        r config set bitmap-roaring-enabled yes
        r del k
        r setbit k 70000 1
        r setbit k 3 1
        set value [r get k]
        set dump [r dump k]
        r del k
        r restore k 0 $dump
        assert_equal $value [r get k]
    }

    test {Roaring: DEBUG RELOAD keeps the value} {
        r config set bitmap-roaring-enabled yes
        r del k
        r setbit k 1234567 1
        r setbit k 12 1
        set value [r get k]
        set digest [debug_digest_value k]
        r debug reload
        assert_equal $value [r get k]
        assert_equal $digest [debug_digest_value k]
        # Loaded as a plain string, converted again by the next bit write.
        r setbit k 13 1
        assert_encoding roaring k
    } {} {needs:debug}

    test {Roaring: sparse bitmaps use little memory} {
        r config set bitmap-roaring-enabled yes
        r del k
        r setbit k [expr {(1 << 32) - 1}] 1
        assert_encoding roaring k
        assert_equal [expr {1 << 29}] [r strlen k]
        assert_range [r memory usage k] 1 1000
        assert_equal 1 [r bitcount k]
        assert_equal [expr {(1 << 32) - 1}] [r bitpos k 1]
        r del k
    }

    test {Roaring: random commands match the plain encoding} {
        r del plain roar
        for {set i 0} {$i < 3000} {incr i} {
            set bit [expr {int(rand() * (rand() < 0.5 ? 1000 : 3000000))}]
            switch [expr {int(rand() * 6)}] {
                0 { roaring_twin setbit KEY $bit [expr {int(rand() * 2)}] }
                1 { roaring_twin getbit KEY $bit }
                2 { roaring_twin bitcount KEY [expr {$bit / 64}] [expr {$bit / 8}] }
                3 { roaring_twin bitpos KEY [expr {int(rand() * 2)}] [expr {$bit / 64}] }
                4 { roaring_twin bitfield KEY incrby u13 $bit 1000 get i29 $bit }
                5 { roaring_twin getrange KEY [expr {$bit / 64}] [expr {$bit / 8}] }
            }
        }
        roaring_assert_same_value
    }
}

start_server {tags {"bitops roaring repl external:skip"}} {
    start_server {} {
        set primary [srv -1 client]
        set primary_host [srv -1 host]
        set primary_port [srv -1 port]
        set replica [srv 0 client]

        test {Roaring: a replica holds the same values, whatever its encoding} {
            $replica replicaof $primary_host $primary_port
            wait_for_condition 50 100 {
                [s 0 master_link_status] eq {up}
            } else {
                fail "Replication not started."
            }
            $primary config set bitmap-roaring-enabled yes
            $replica config set bitmap-roaring-enabled no
            for {set i 0} {$i < 1000} {incr i} {
                $primary setbit k [expr {int(rand() * 1000000)}] 1
            }
            $primary bitop not k2 k
            wait_for_ofs_sync $primary $replica
            assert_equal [$primary object encoding k] roaring
            assert_equal [$replica object encoding k] raw
            assert_equal [$primary debug digest] [$replica debug digest]
        } {} {needs:debug}
    }
}

--
-- SHA2 family
--

-- SHA224
SELECT digest('', 'sha224');
SELECT digest('a', 'sha224');
SELECT digest('abc', 'sha224');
SELECT digest('abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq', 'sha224');
SELECT digest('12345678901234567890123456789012345678901234567890123456789012345678901234567890', 'sha224');

-- SHA256
SELECT digest('', 'sha256');
SELECT digest('a', 'sha256');
SELECT digest('abc', 'sha256');
SELECT digest('abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq', 'sha256');
SELECT digest('12345678901234567890123456789012345678901234567890123456789012345678901234567890', 'sha256');

-- SHA384
SELECT digest('', 'sha384');
SELECT digest('a', 'sha384');
SELECT digest('abc', 'sha384');
SELECT digest('abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq', 'sha384');
SELECT digest('abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmnhijklmnoijklmnopjklmnopqklmnopqrlmnopqrsmnopqrstnopqrstu', 'sha384');
SELECT digest('abcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyz', 'sha384');

-- SHA512
<<<<<<< HEAD
SELECT encode(digest('', 'sha512'), 'hex');
SELECT encode(digest('a', 'sha512'), 'hex');
SELECT encode(digest('abc', 'sha512'), 'hex');
SELECT encode(digest('abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq', 'sha512'), 'hex');
SELECT encode(digest('abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmnhijklmnoijklmnopjklmnopqklmnopqrlmnopqrsmnopqrstnopqrstu', 'sha512'), 'hex');
SELECT encode(digest('abcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyz', 'sha512'), 'hex');

-- SM3 
-- `icw_bash` will used `--with-openssl` version.
-- but openssl version which cbdb required have not SM2/SM3/SM4
-- start_ignore
SELECT encode(digest('', 'sm3'), 'hex');
SELECT encode(digest('a', 'sm3'), 'hex');
SELECT encode(digest('abc', 'sm3'), 'hex');
SELECT encode(digest('abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq', 'sm3'), 'hex');
SELECT encode(digest('abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmnhijklmnoijklmnopjklmnopqklmnopqrlmnopqrsmnopqrstnopqrstu', 'sm3'), 'hex');
SELECT encode(digest('abcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyz', 'sm3'), 'hex');
-- end_ignore
=======
SELECT digest('', 'sha512');
SELECT digest('a', 'sha512');
SELECT digest('abc', 'sha512');
SELECT digest('abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq', 'sha512');
SELECT digest('abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmnhijklmnoijklmnopjklmnopqklmnopqrlmnopqrsmnopqrstnopqrstu', 'sha512');
SELECT digest('abcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyz', 'sha512');
>>>>>>> REL_16_9

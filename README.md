# discard0

Tool for discarding block device ranges containing only zeroes

This Linux utitity scans a block device for ranges of blocks containing
only zeroes, and if it finds any, issues a `BLKDISCARD` ioctl to
get rid of them.

It is similar to the **fstrim** tool for file systems. The intended use of
**discard0** is for block devices that are not used as file systems directly
(e.g. raw devices used by virtual machines). If in doubt, use **fstrim**!

## CAUTION - CAUTION - CAUTION ##

__THIS TOOL MAY DESTROY YOUR DATA!__

Some block devices consistently return zeroes on discarded blocks.
Using this utility is only safe on devices which consistently return zeroes
on discarded blocks.

I wrote this utility for discarding zero-blocks on a thin volumes. It *should*
be safe for LVM thin, because the LVM thin code consistently returns zeroes
for unmapped blocks. It worked well in my tests.

__But there is NO GUARANTEE. Backup your data before using this. YOU HAVE BEEN WARNED!__

## Questions and anwers

Q: Why does the tool use `BLKDISCARD` and not `fallocate(FALLOC_FL_PUNCH_HOLE)`?

A: It seems that currently (mid 2018), not all devices that return
zeroes consistently after a discard report this correctly to the kernel (sysfs
attribute `queue/write_zeroes_max_bytes`). In particular, **dm-thin** doesn't
advertize support for this feature, although it quite obviously has
it. Therefore `fallocate(FALLOC_FL_PUNCH_HOLE)` wouldn't work on LVM thin
provisioning devices.

# ddb - distributed device backup

The purpose of the ddb suite is to efficiently copy data between devices,
independently on their location on the network.  It is also good for salvaging
data from faulty devices.

The concept of a device is rather more general than what is understood by the
operating system or by utilities like dd in the sense that a device could mean a
local device as known to the operating system, a remote device as known to a
different operating system running on a remote system, a collection of devices
joined together in some way, or finally a program running somewhere and
communicating with ddb using the documented API.

A device is identified by a type and a name; in some cases, the type can be
omitted, and ddb guesses it (autodetection) although when using ddb from
scripts, or using the API it is recommended to always specify a type if known,
to avoid the possibility of autodetection making a wrong guess.

More specifically, a device can be one of:

### regular device
This is a device as the operating system knows it, normally specified as
a path starting with "/dev/". The type is normally autodetected but can
be specified as "device", for example to avoid creating a regular file in
"/dev" when trying to write to a device which does not exist.

### image
This is a regular file containing data from a device; if supported by the
operating system, it will be a sparse file; normally specified as the full
path to the file. The type will be normally autodetected, but can be
specified as "image", for example to refuse writing to a device instead.

### image with metadata
Similar to an image, but instead of using a sparse file it stores information
about which blocks are present in the file itself. The type is normally
autodetected for an existing file, but needs to be specified as "meta"
to create a new file; also, specifying the type for an existing file
will make sure that an "image" won't be used instead if the file does not
have a metadata header.  See **ddb-meta (7)**
for information on how to specify this type of device and the options available.

### remote
This is a device accessed using a generic interface and set up using a
configuration file; the transport can be a pipe to another program or a TCP
connection.  The type of each remote device is specified by the configuration,
which also specifies whether autodetection is permitted.  See **ddb-remote (7)**
for information about this device and **ddb (5)** for information on how to set
up this type of device.

### module
This is a device accessed by loading a shared library object and calling a
function within it; this is set up using a configuration file.  The type of each
module device is specified by the configuration, which also specifies whether
autodetection is permitted.  See **ddb-module (7)** for information about this
device and **ddb (5)** for information on how to set up this type of device.

### sequence
A sequence of images with metadata representing an initial full copy of a device
and a number of incremental backups; this is normally specified as a path to the
directory, and the program will read all images contained in there. The type of
a sequence device is "sequence", although using a directory instead of a file
will autodetect this type. See **ddb-sequence (7)** for more information, and
for a description of tools available to manage this type of device.

### lvm
Similar to a plain block device, but with some extra knowledge of LVM, for
example to handle snapshots automatically. The type of this device is "lvm", and
it can be autodetected when the name is a correct device name known to LVM. See
**ddb-lvm (7)** for more information.

### error
Opens another device and simulates random errors on it. For testing only.  See
**ddb-error (7)** for more information.

## EXAMPLES

The manual page for **ddb (1)** provides a full description of the utility; in its
simplest form, it accepts two devices, a source and a destination, and copies
data from one to the other, so for example:

`ddb /dev/sda1 /var/backup/sda1`

Creates a disk image of /dev/sda1, while

`ddb /dev/sda1 /var/backup/sda1/`

Creates a full backup of /dev/sda1 as an image inside the directory
/var/backup/sda1 using a "sequence" type; if the directory does not exist,
it will be created, but the slash at the end of the name is essential, otherwise
the name will be treated as an image; a second run of the program:

`ddb /dev/sda1 /var/backup/sda1/`

will find the full backup in there and add a new image with the differences
between the device and the full backup, in other words, an incremental backup.

The above can specify a type to make sure that autodetection doesn't
guess wrongly:

`ddb - device - sequence /dev/sda1 /var/backup/sda1/`

To restore from a backup:

`ddb /var/backup/sda1/ /dev/sda1`

This will use the latest incremental backup; a special syntax (see ddb-sequence
(7) for information) allows to access an earlier backup, for example:

`ddb /var/backup/sda1/2017-12-01 /dev/sda1`

Restores /dev/sda1 to the latest backup on or before December 1st 2017.

In the above examples, either the source or the destination can be remote,
by providing appropriate configuration; see **ddb (5)** for example of using remote devices.


Please see the man pages for more information:

dd (1),
ddb (1),
ddb (5),
ddb-error (7),
ddb-lvm (7),
ddb-meta (7),
ddb-module (7),
ddb-remote (7),
ddb-sequence (7)

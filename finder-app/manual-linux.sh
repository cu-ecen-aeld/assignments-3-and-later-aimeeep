#!/bin/bash
# Script outline to install and build kernel.
# Author: Siddhant Jajoo.

set -e
set -u

OUTDIR=/tmp/aeld
KERNEL_REPO=git://git.kernel.org/pub/scm/linux/kernel/git/stable/linux-stable.git
KERNEL_VERSION=v5.1.10
BUSYBOX_VERSION=1_33_1
FINDER_APP_DIR=$(realpath $(dirname $0))
ARCH=arm64
CROSS_COMPILE=aarch64-none-linux-gnu-

if [ $# -lt 1 ]
then
	echo "Using default directory ${OUTDIR} for output"
else
	OUTDIR=$1
	echo "Using passed directory ${OUTDIR} for output"
fi

mkdir -p ${OUTDIR}

if [ ! -d "${OUTDIR}" ]; then
	echo "Failed to create ${OUTDIR}"
	exit 1
fi

cd "$OUTDIR"
if [ ! -d "${OUTDIR}/linux-stable" ]; then
    #Clone only if the repository does not exist.
	echo "CLONING GIT LINUX STABLE VERSION ${KERNEL_VERSION} IN ${OUTDIR}"
	git clone ${KERNEL_REPO} --depth 1 --single-branch --branch ${KERNEL_VERSION}
fi
if [ ! -e ${OUTDIR}/linux-stable/arch/${ARCH}/boot/Image ]; then
    cd linux-stable
    echo "Checking out version ${KERNEL_VERSION}"
    git checkout ${KERNEL_VERSION}

    # TODO: Add your kernel build steps here
    make ARCH=${ARCH} CROSS_COMPILE=${CROSS_COMPILE} mrproper #deep clean
    make ARCH=${ARCH} CROSS_COMPILE=${CROSS_COMPILE} defconfig #configure for "virt" arm dev board
    make -j4 ARCH=${ARCH} CROSS_COMPILE=${CROSS_COMPILE} all #kernel image for booting with QEMU
    #make ARCH=${ARCH} CROSS_COMPILE=${CROSS_COMPILE} modules #any kernel modules
    make ARCH=${ARCH} CROSS_COMPILE=${CROSS_COMPILE} dtbs #decivetree
fi

echo "Adding the Image in outdir"

echo "Creating the staging directory for the root filesystem"
cd "$OUTDIR"
if [ -d "${OUTDIR}/rootfs" ]
then
	echo "Deleting rootfs directory at ${OUTDIR}/rootfs and starting over"
    sudo rm  -rf ${OUTDIR}/rootfs
fi

# TODO: Create necessary base directories
# Create rootfs directory and folder tree
sudo mkdir -p ${OUTDIR}/rootfs
cd "${OUTDIR}/rootfs"
sudo mkdir -p bin dev etc home lib lib64 proc sbin sys tmp user var
sudo mkdir -p usr/bin usr/lib usr/sbin
sudo mkdir -p var/log
sudo chmod -R 777 ${OUTDIR}/rootfs

cd "$OUTDIR"
if [ ! -d "${OUTDIR}/busybox" ]
then
git clone git://busybox.net/busybox.git
    cd busybox
    git checkout ${BUSYBOX_VERSION}
    # TODO:  Configure busybox
    sudo chmod -R 755 ${OUTDIR}/busybox
else
    cd busybox
fi

# TODO: Make and install busybox
sudo make distclean
sudo make defconfig
make ARCH=${ARCH} CROSS_COMPILE=${CROSS_COMPILE}
make CROSS_COMPILE="$CROSS_COMPILE" CONFIG_PREFIX="${OUTDIR}/rootfs" install
cd ${OUTDIR}/rootfs

echo "Library dependencies"
${CROSS_COMPILE}readelf -a bin/busybox | grep "program interpreter"
${CROSS_COMPILE}readelf -a bin/busybox | grep "Shared library"

# TODO: Add library dependencies to rootfs
SYSROOT=$(aarch64-none-linux-gnu-gcc -print-sysroot)
dep1=$(find $SYSROOT -name "libm.so.6")
dep2=$(find $SYSROOT -name "libresolv.so.2")
dep3=$(find $SYSROOT -name "libc.so.6")
dep4=$(find $SYSROOT -name "ld-linux-aarch64.so.1")

cp ${dep1} ${dep2} ${dep3} ${OUTDIR}/rootfs/lib64
cp ${dep4} ${OUTDIR}/rootfs/lib

# TODO: Make device nodes
cd "${OUTDIR}/rootfs"
sudo mknod -m 666 dev/null c 1 3
sudo mknod -m 666 dev/console c 5 1


# TODO: Clean and build the writer utility

cd ${FINDER_APP_DIR}
sudo chmod 777 ${FINDER_APP_DIR}
sudo make clean
make CROSS_COMPILE=${CROSS_COMPILE}

# TODO: Copy the finder related scripts and executables to the /home directory
# on the target rootfs
sudo cp ${FINDER_APP_DIR}/finder.sh ${FINDER_APP_DIR}/finder-test.sh ${FINDER_APP_DIR}/autorun-qemu.sh ${OUTDIR}/rootfs/home
sudo mkdir -p ${OUTDIR}/rootfs/home/conf
sudo cp ${FINDER_APP_DIR}/conf/*.txt ${OUTDIR}/rootfs/home/conf

# TODO: Chown the root directory
cd ${OUTDIR}/rootfs
sudo chown -R root:root *

# TODO: Create initramfs.cpio.gz
cd "${OUTDIR}/rootfs"
find . | cpio -H newc -ov --owner root:root > ${OUTDIR}/initramfs.cpio
gzip -f ${OUTDIR}/initramfs.cpio


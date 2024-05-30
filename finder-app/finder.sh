#!/bin/bash
 
if [ $# -ne 2 ];then
	echo "Illegal number of parameters"
	exit 1
fi

filesdir=$1
searchstr=$2

if [ ! -d $filesdir ]; then
	echo "does not represent a directory on the filesystem"
	exit 1
fi

filescount=$(find $filesdir -type f | wc -l)
matchlines=$(grep -r $searchstr $filesdir | wc -l)

echo "The number of files are $filescount and the number of matching lines are $matchlines"

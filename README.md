# winexe
Remote Windows command on Linux System


Repository 

yum install autoconf

yum install gcc

yum install python

yum install python-devel


How to install:

You can download the source package from here [Current version is winexe-1.00.tar.gz]

1. tar -xvf winexe-1.00.tar.gz

2. cd winexe-1.00/source4/

3. ./autogen.sh

4. ./configure

5. make basics bin/winexe

6. make “CPP=gcc -E -ffreestanding” basics bin/winexe (For X64 bit)


Sample Usage

./winexe -A credentials.cfg //192.168.1.100 'cmd.exe /c ipconfig /all'

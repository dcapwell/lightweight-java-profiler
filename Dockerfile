FROM centos:centos6

# Setup gcc to compile profiler
# http://superuser.com/questions/381160/how-to-install-gcc-4-7-x-4-8-x-on-centos
RUN \
  yum install -y wget which java-1.7.0-openjdk-devel.x86_64 && \
  cd /etc/yum.repos.d && \
  wget http://people.centos.org/tru/devtools-1.1/devtools-1.1.repo && \
  yum --enablerepo=testing-1.1-devtools-6 install -y devtoolset-1.1-gcc devtoolset-1.1-gcc-c++

ENV CC    /opt/centos/devtoolset-1.1/root/usr/bin/gcc  
ENV CPP   /opt/centos/devtoolset-1.1/root/usr/bin/cpp
ENV CXX   /opt/centos/devtoolset-1.1/root/usr/bin/c++
ENV PATH  /usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin:/opt/centos/devtoolset-1.1/root/usr/bin

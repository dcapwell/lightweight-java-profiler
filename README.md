lightweight-java-profiler
=========================

> A Lightweight profiling tool for Java

This is a proof of concept implementation for a lightweight Java profiler. It is a sampling profiler that gathers stack traces asynchronously, avoiding the inaccuracies of only being able to profile code at safe points, and the overhead of having to stop the JVM to gather a stack trace. The result is a more accurate profiler that avoids the 10-20% overhead of something like hprof.

For more about why this is interesting, see

http://jeremymanson.blogspot.com/2010/07/why-many-profilers-have-serious.html

This profiler will only work with OpenJDK derived JDK/JVMs. To get started, see the getting started page.

Fork of https://code.google.com/p/lightweight-java-profiler/

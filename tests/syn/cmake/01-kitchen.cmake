# CMake kitchen sink
cmake_minimum_required(VERSION 3.24)
project(Yew VERSION 1.0)
$<BUILD_INTERFACE:include>
if(ON)
  set(NAME "${PROJECT_NAME}-$<CONFIG>\n")
elseif(FALSE)
  message(STATUS $ENV{HOME} $CACHE{ENTRY})
else()
  continue()
endif()
foreach(item IN ITEMS 1 2.5)
  break()
endforeach()

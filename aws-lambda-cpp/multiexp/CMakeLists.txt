cmake_minimum_required(VERSION 3.5)
set(CMAKE_CXX_STANDARD 11)
project(multiexp LANGUAGES CXX)
#find_package(aws-utils REQUIRED)

find_package(aws-lambda-runtime REQUIRED)
find_package(OpenSSL REQUIRED)
#find_package(libprocps REQUIRED)
#find_package(libff.a REQUIRED)
#SET(CMAKE_CXX_FLAGS "-NO_PROCPS=1")
#SET(CMAKE_EXE_LINKER_FLAGS "-NO_PROCPS=1")

add_library(aws-core SHARED IMPORTED)
set_target_properties(aws-core PROPERTIES IMPORTED_LOCATION "/usr/local/lib/libaws-cpp-sdk-core.so")
add_definitions(-DNO_PROCPS=1)
add_executable(${PROJECT_NAME} "main.cpp")
target_link_libraries(${PROJECT_NAME} PUBLIC libff.a AWS::aws-lambda-runtime gmp libzm.a OpenSSL::SSL aws-core) # libprocps)
aws_lambda_package_target(${PROJECT_NAME})

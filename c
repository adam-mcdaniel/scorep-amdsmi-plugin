if [ -z "$INSTALL_DIR" ]; then
    echo "The INSTALL_DIR is not set."
    echo "Please set the INSTALL_DIR environment variable to the install/ directory"
    exit 1
fi

source $INSTALL_DIR/../setup-env.sh
if [ $? -ne 0 ]; then
    echo "Failed to source the setup-env.sh relative to INSTALL_DIR."
    exit 1
fi

# 2. Matamos temporalmente las variables globales que están envenenando a CMake
unset CFLAGS
unset CXXFLAGS
unset LDFLAGS
unset CPPFLAGS
unset CC
unset CXX

# ¡AQUÍ ESTÁ LA MAGIA! Destruimos la caché corrupta antes de reconfigurar
rm -rf CMakeCache.txt CMakeFiles/ cmake_install.cmake

# 3. Llamamos a CMake forzando a Clang de manera estricta
cmake .. \
    -DCMAKE_C_COMPILER=/opt/rocm-7.2.0/llvm/bin/clang \
    -DCMAKE_CXX_COMPILER=/opt/rocm-7.2.0/llvm/bin/clang++ \
    -DCMAKE_INSTALL_PREFIX=/home/hom/scorep-amd/install \
    -DCMAKE_BUILD_TYPE=Release \
    
echo "Installing to $INSTALL_DIR"
gmake clean
gmake
gmake install
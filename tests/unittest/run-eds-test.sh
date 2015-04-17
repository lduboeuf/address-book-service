#!/bin/sh

echo ARG0=$0 # this script
echo ARG1=$1 # full executable path of dbus-test-runner
echo ARG2=$2 # full executable path of test app
echo ARG3=$3 # test name
echo ARG4=$4 # full executable path of evolution-addressbook-factory
echo ARG5=$5 # bus service name of AddressBook factory
echo ARG6=$6 # full exectuable path of qtpim-service    

# set up the tmpdir and tell the shell to purge it when we exit
export TEST_TMP_DIR=$(mktemp -p "${TMPDIR:-/tmp}" -d $3-XXXXXXXXXX) || exit 1
echo "running test '$3' in ${TEST_TMP_DIR}"

# set up the environment variables
export QT_QPA_PLATFORM=minimal
export FOLKS_BACKENDS_ALLOWED=eds
export HOME=${TEST_TMP_DIR}
export XDG_RUNTIME_DIR=${TEST_TMP_DIR}
export XDG_CACHE_HOME=${TEST_TMP_DIR}/.cache
export XDG_CONFIG_HOME=${TEST_TMP_DIR}/.config
export XDG_DATA_HOME=${TEST_TMP_DIR}/.local/share
export XDG_DESKTOP_DIR=${TEST_TMP_DIR}
export XDG_DOCUMENTS_DIR=${TEST_TMP_DIR}
export XDG_DOWNLOAD_DIR=${TEST_TMP_DIR}
export XDG_MUSIC_DIR=${TEST_TMP_DIR}
export XDG_PICTURES_DIR=${TEST_TMP_DIR}
export XDG_PUBLICSHARE_DIR=${TEST_TMP_DIR}
export XDG_TEMPLATES_DIR=${TEST_TMP_DIR}
export XDG_VIDEOS_DIR=${TEST_TMP_DIR}
export ADDRESS_BOOK_SERVICE_DEBUG=On
export GIO_USE_VFS=local # needed to ensure GVFS shuts down cleanly after the test is over

echo HOMEDIR=${HOME}
rm -rf ${XDG_DATA_HOME}

# run dbus-test-runner
$1 --keep-env --max-wait=90 \
--task $2 --task-name $3 --wait-until-complete --wait-for=com.canonical.pim \
--task $6 --task-name "ubuntu-pim" --wait-for=$5  -r \
--task $4 --task-name "evolution-addressbook" -r

rv=$?

# if the test passed, blow away the tmpdir
if [ $rv -eq 0 ]; then
    rm -rf $TEST_TMP_DIR
fi

return $rv

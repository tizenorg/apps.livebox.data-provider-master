#!/bin/sh

DBOX_DEBUG=$1/dynamicbox
mkdir -p ${DBOX_DEBUG}
/bin/cp -r /opt/usr/share/live_magazine ${DBOX_DEBUG}
/bin/cp -r /tmp/.dbox.service ${DBOX_DEBUG}/log
/bin/cp -r /opt/dbspace/.dynamicbox.db ${DBOX_DEBUG}
ls -la /tmp/ | /bin/grep srw > ${DBOX_DEBUG}/log/tmp.hidden_files

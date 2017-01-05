
TEMPLATE = subdirs

!tools-only {
    # Although the testrunner is in tools we don't want to build it with tools-only
    # because it is based on the manager binary
    SUBDIRS += \
        testrunner
}

!android:SUBDIRS += \
    packager \
    deployer \

qtHaveModule(dbus):SUBDIRS += \
    controller \

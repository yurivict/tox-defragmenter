# tox-defragmenter
Tox module allowing to send messages of unlimited size.

# Why tox-defragmenter?
Due to the low-level nature of the Tox library, Tox API allows to send messages of size limited to TOX_MAX_MESSAGE_LENGTH, which is 1372 bytes. Tox clients need to split longer messages, such that the individual parts don't exceed 1372 bytes. This causes long messages to appear as a sequence of many messages. This is the undesirable effect, because users expect their messages to be received in the same form they were sent.

# How does it work?
tox-defragmenter is between the Tox client and the Tox library. When it sees the long message, it splits it in fragments and adds the special marker to the parts so that they can be identified as fragments, and not an independent messages. On the receiving end, tox-defragmenter recombines the fragments into the original message that it then sends to the client when all fragments have arrived.

# API
tox-defragmenter API requires two intialization functions to be called: tox_defragmenter_initialize_api and tox_defragmenter_initialize_db before it can be used. It also requires the function tox_defragmenter_periodic to be called every few seconds, and the function tox_defragmenter_uninitialize to be called in the end.

For clients that don't use SQLite or sqlcipher tox-defragmenter can create in-memory database. This will lose the ability to send long messages persistently across sessions, and client restart on any end will require to re-send all messages. In-memory database should be initialized with tox_defragmenter_initialize_db_inmemory.

# Dependencies
* Build-time dependency on the tox library.
* Expects the caller to depend on SQLite or sqlcipher.

# Caveats
* Due to the SQLite blob bug discovered during the development process, tox-defragmenter has to open and close db blobs for each fragment, which causes the performance impact on the receiving end. Until this SQLite bug is fixed, only moderately long messages can be sent.

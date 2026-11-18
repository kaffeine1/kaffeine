Subject: [PATCH] m68kemu: harden HUNK and 68k library loading, and give a child's seglist copy a lifetime

This series makes m68kemu refuse malformed hunk files and 68k disk libraries
instead of reaching outside its containment. When the library loader refuses
a library, it gives back exactly what that attempt took.

It also gives an owner to the copy that `CreateNewProc` makes of a 68k
`LoadSeg()` seglist:

- A child that is cancelled, or that cannot be started, gives the copy back at
  once.
- A child whose first active `NP_FreeSeglist` is TRUE frees it when it ends,
  as dos would unload the seglist.
- With FALSE, and for `CreateProc`, the copy stays, because the program may
  still use addresses inside it.

The tags themselves still reach dos unchanged.

Three host regressions are new: library loading bounds, the hunk file loader
and the seglist copy. The child projection now has 151 checks. All of them
pass on the host, the child projection also under ASan and UBSan, and four
negative controls fail exactly the checks they should.

This series has not been built or run on AROS. A FALSE or `CreateProc` copy is
still kept until the program ends. The details are in the README.

Base: 10ff84d828ff9a074b7d74fb753e3bd731a421a5.

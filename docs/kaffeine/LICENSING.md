# Licenses and provenance of kaffeine

This guide summarises the audit of the M0 base and suggests precautions
for the rest of the project. The license texts and the notices of the
individual files are authoritative; the guide grants no additional rights
and does not certify the compliance of a future distribution.

## Components and reviewed revisions

The AROS base of M0 is commit
`ae45f4644f80de542fb6af320c5d5fb5a1bc9925`.

| Component | License and scope | Status in the project |
|---|---|---|
| AROS code of `kaffeine.library` and the project's changes | [AROS Public License 1.1](../../LICENSE), APL | Base, the M0 changes to the CPU interface and the dispatcher, and later changes such as the seglist relocation record in `dos.library`. APL is distinct from MPL. |
| Moira CPU core | [MIT, text at the reviewed commit](https://github.com/dirkwhoffmann/Moira/blob/1efd69467ca13b27b2fb40febd5cb31dbecdea5f/Moira/LICENSE) | Only CPU backend of M0; revision `1efd69467ca13b27b2fb40febd5cb31dbecdea5f`. The MIT qualification covers the core, not every tool in the Moira repository. |
| Emu68 | [Mozilla Public License 2.0, text at the reviewed commit](https://github.com/michalsc/Emu68/blob/2fb0e320f30cf9c47fa250cd84d2bcfff97d40fc/LICENSE) | Sources reviewed at revision `2fb0e320f30cf9c47fa250cd84d2bcfff97d40fc` as the reference for the kaffeine JIT, the translator, which is written for this project; no Emu68 code is in the repository. |
| Other components and dependencies | Their own licenses and notices | The AROS [LEGAL](../../LEGAL) file lists exceptions, including GPL and LGPL; the individual subcomponents actually distributed must be checked as well. |

The [Emu68 AUTHORS file](https://github.com/michalsc/Emu68/blob/2fb0e320f30cf9c47fa250cd84d2bcfff97d40fc/AUTHORS)
also refers to the specific notices of third-party components. Should
any part of Emu68 ever be incorporated, the proposal is to record the
subset of the engine and of the dependencies actually incorporated, with
their license texts.

## If Emu68 files were ever imported

The kaffeine JIT, the translator of this project, is written for it and
does not import Emu68. Should that change, APL §3.7 and MPL 2.0 §3.3 allow a combined
work, called a *Larger Work*, provided the obligations for each part are
met. The proposal is to keep the AROS files covered by APL separate from
any Emu68 files covered by MPL 2.0, preserving their licenses and
connecting them through the backend interface. Every transfer of code between files has to be checked:
changes and copies can carry the obligations of the original license with
them.

This arrangement does not authorise a global relicensing under MPL or GPL.
[LICENSE.Author](../../LICENSE.Author) grants alternatives only for the
changesets of the listed authors, without replacing the license of the
whole file or project. Attributions and contributions must be documented
correctly, without assuming that authors have assigned their rights to the
project.

## Private work and delivery to external recipients

Private development does not require publishing the whole repository.
Delivery to third parties, even free of charge or through a private
repository, does require meeting the obligations for the copies provided.
For APL or MPL binaries, recipients must be able to obtain the covered
sources under the respective licenses; this does not require publishing
the whole repository.

APL §3.2 allows the changes to be provided in source form on the same
media as the binaries or through an electronic distribution mechanism. In
the latter case they must remain available for at least **12 months after
they first became available**, or for at least **6 months after a later
version of that particular change has been made available to the
recipients**. The distributor remains responsible even if the service is
run by a third party. MPL 2.0 §3.2 requires access to the sources by
reasonable means and in a timely manner, at a charge no more than the
cost of distribution.

## Proposed release preparation

Before distributing new sources or binaries, the proposal is to check:

1. **Licenses and attributions:** include the required texts and notices,
   keep the upstream ones and correctly distinguish new contributions.
   For the Moira core, keep the copyright and MIT notice in distributed
   copies or substantial portions.
2. **Changes and dates:** describe the changes and the derivation from the
   original base, as required by APL §3.3. Check the APL notices of
   Exhibit A according to §3.5, as well as the notices of the other
   licenses.
3. **Versions and rebuilding:** record the base commit, dependencies and
   patches; provide the relevant sources, interfaces and the scripts needed
   to compile and install, with reproducible instructions.
4. **Source access:** tell recipients where to obtain the version that
   matches the binaries and keep it available under the applicable terms.
   A link to the current upstream branch alone can lose that
   correspondence.
5. **Added components:** review licenses, notices and provenance again
   when a dependency changes or tools and materials are added to the
   delivery.

These are criteria for the next deliveries, not a statement that every
check has already been made for a future release.

## Open point: AmigaOS NDK 3.2 provenance

The [upstream m68kemu README at the reviewed base](https://github.com/aros-development-team/AROS/blob/ae45f4644f80de542fb6af320c5d5fb5a1bc9925/rom/m68kemu/README.md)
states that Hyperion's AmigaOS NDK 3.2 was used for the structure
definitions. The upstream offset generator reads NDK headers; the tables
contain structure and field names, sizes and offsets. It is therefore
advisable to verify and document the provenance and terms of use of the
generators' inputs and outputs, distinguishing thunks, offsets and
structure descriptions by their actual sources.

M0 does not import the NDK and does not regenerate these tables. The audit
certifies neither lawfulness nor an infringement concerning the derived
data. For a release that includes the already generated materials, and for
any new import or regeneration, the applicable NDK license text and the
content actually redistributed still have to be checked.

## Primary references

- **APL 1.1:** [text in the repository](../../LICENSE) and
  [official AROS text](https://aros.sourceforge.io/license.html).
  Relevant sections: §1.9, modifications; §1.11, source code; §§3.1-3.3,
  distribution and documentation; §§3.5-3.7, notices, executable versions
  and larger works; §5.2, application to the source tree unless a
  different license is explicit; §§6 and 13, license versions and
  alternative permissions.
- **MPL 2.0:** [official Mozilla text](https://www.mozilla.org/en-US/MPL/2.0/),
  §§1.7 and 1.10, larger work and modifications; §§3.1-3.4, distribution
  and notices. The [official FAQ, Q11](https://www.mozilla.org/en-US/MPL/2.0/FAQ/)
  clarifies the boundary between covered files and the other files of a
  larger work.
- **Moira and Emu68:** the links in the table point to the texts of the
  reviewed revisions, without depending on the local initialisation of the
  submodules.
- **AROS exceptions and permissions:** [LEGAL](../../LEGAL) and
  [LICENSE.Author](../../LICENSE.Author), to be read together with the
  local notices.

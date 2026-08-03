# Getting access to research papers

> **Status:** Current
> **Owner:** Research
> **Last reviewed:** 2026-08-02
> **Related:** RESEARCH-PLAN.md, BIBLIOGRAPHY.md

Practical workflow for obtaining primary sources. Written because "the paper is paywalled"
is almost never the end of the story, and because the project's evidence rules
(CONTRIBUTING.md) mean a paper we cannot read cannot be used as evidence.

## Who ACM and IEEE are

Two professional societies that between them publish most computing and
electrical-engineering research:

- **ACM** (Association for Computing Machinery) — runs MobiCom, MobiSys, UbiComp/IMWUT,
  SIGCOMM. Archive: the **ACM Digital Library** (`dl.acm.org`).
- **IEEE** (Institute of Electrical and Electronics Engineers) — runs INFOCOM, ICDCS, and
  publishes the Transactions journals. Archive: **IEEE Xplore** (`ieeexplore.ieee.org`).

Both charge for access. That is what the HTTP 403 on the ShiftCode paper was.

## The access ladder — work down it in order

**1. The authors' own web page.** Academics routinely post PDFs of their own papers, and
publisher agreements generally permit self-archiving the accepted version. This is legal,
free, and it is how we obtained **three of our five most valuable sources**:

| Paper | Where we actually got it |
|---|---|
| ChromaCode (MobiCom 2018) | `cs.purdue.edu/homes/chunyi/pubs/` |
| Spatially Adaptive Embedding (INFOCOM 2016) | `winlab.rutgers.edu/~gruteser/papers/` |
| Visual MIMO ×3 (CISS/MobiCom) | `winlab.rutgers.edu/~aashok/` and `~gruteser/` |

**The search that works:** exact paper title in quotes, plus `pdf`, and look for `.edu`,
`.ac.uk` or lab-hostname results. Author publication lists (`~name/Publications.html`) are
often a goldmine — the Rutgers WINLAB page had an entire research programme's worth.

**2. arXiv** (`arxiv.org`) — free preprints. Common in ML and networking, less so in mobile
systems, but always worth a check.

**3. Google Scholar → "All N versions".** That link expands to every mirror Scholar knows
about, frequently including a free institutional copy.

**4. Unpaywall / Open Access Button / CORE.** Services that map a DOI to a legal free copy.
Unpaywall has a browser extension that puts a green tab on paywalled pages when one exists.

**5. Semantic Scholar** (`semanticscholar.org`) — often hosts PDFs directly and has a
usable free API.

**6. Email the authors.** Genuinely effective and completely normal — researchers are
usually pleased someone wants to read their work, and a one-line request gets a PDF more
often than not. For ShiftCode and RDCode this is the realistic next step.

**7. Institutional access.** A university affiliation, or in some countries a national or
municipal library card, gets you the full libraries. Some public library systems carry IEEE
Xplore.

**8. Paid individual access.** ACM membership plus DL access, or IEEE membership plus
Xplore, run to a few hundred dollars a year. Individual papers are typically $15–35.
**I would not spend this yet** — see the recommendation below.

## What I will not do

I will not use Sci-Hub, LibGen or similar. They distribute copyrighted work without
permission, and the ladder above has been sufficient so far.

## Recommendation for this project

**Do not buy anything yet.** Of the seven papers on the reading queue, we have obtained
five free, and the two remaining (ShiftCode, RDCode) are best pursued by emailing the
authors. If that fails and they still look decisive, a single-paper purchase is a
reasonable ~$20.

A more useful investment than paper access would be **reference hardware and a rigid
mounting rig** — our biggest gaps (`Pc`, `Fd`, the density cliff) are things no paper can
tell us, because no published work uses our exact device pair.

## For agent-driven research specifically

What works well when I do this:
- I can fetch and read any openly-hosted PDF, extract its text, and pull out real numbers.
- I cannot get past authentication walls, and I will not try.
- Search-result summaries are **pointers, not evidence** — the bibliography's `Access`
  field enforces this, and rows marked `secondary` may not back a design decision.

If you ever do get institutional access, the highest-value thing you could hand me is
**PDFs of the four visible-branch papers** (COBRA, RainBar, RDCode, ShiftCode). That is the
single biggest remaining hole in the foundation — it is our project's direct lineage and we
currently have no primary numbers from any of it.

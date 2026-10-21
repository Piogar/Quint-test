# Shared settings for scripts/*.sh (sourced, not executed).

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
SPEC="$ROOT/spec/stats.qnt"
MAIN=StatsFrameworkVerification

# All witnesses W-01 … W-07 (val names in StatsFrameworkVerification).
# Witnesses cannot be grouped in Quint (conclusions.md, 3.5), so this is the
# single list used by gen_traces.sh and check_all.sh. Add new witnesses here.
WITNESSES="w01HolderFull w02FlushRetried w03InvalidDiscarded w04StopWithRepositoryDown w05FullCycle w06StopDrainsToRepository w07FlusherFull"

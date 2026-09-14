"""Run the production H2P helper white-box checks in the final gem5 binary."""
import m5
from m5.objects import H2PStatTest, Root

root = Root(full_system=False)
root.test = H2PStatTest()
m5.instantiate()
event = m5.simulate()
assert event.getCause() == "H2P self-test passed", event.getCause()
print(event.getCause())

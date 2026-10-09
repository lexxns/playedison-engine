--ハイパーサイコガンナー
function c95526884.initial_effect(c)
	--synchro summon
	aux.AddSynchroProcedure(c,nil,aux.NonTuner(Card.IsRace,RACE_PSYCHO),1)
	c:EnableReviveLimit()
	--pierce
	local e1=Effect.CreateEffect(c)
	e1:SetType(EFFECT_TYPE_SINGLE)
	e1:SetCode(EFFECT_PIERCE)
	c:RegisterEffect(e1)
	--recover
	local e2=Effect.CreateEffect(c)
	e2:SetDescription(aux.Stringid(95526884,0))
	e2:SetCategory(CATEGORY_RECOVER)
	e2:SetProperty(EFFECT_FLAG_PLAYER_TARGET)
	e2:SetType(EFFECT_TYPE_SINGLE+EFFECT_TYPE_TRIGGER_F)
	e2:SetCode(EVENT_DAMAGE_STEP_END)
	e2:SetCondition(c95526884.reccon)
	e2:SetTarget(c95526884.rectg)
	e2:SetOperation(c95526884.recop)
	c:RegisterEffect(e2)
end
function c95526884.reccon(e,tp,eg,ep,ev,re,r,rp)
	local c=e:GetHandler()
	local a=Duel.GetAttacker()
	local d=Duel.GetAttackTarget()
	-- a direct attack has no target, so `d` is nil: indexing it raised
	-- "attempt to index a nil value (local 'd')" at the end of the Damage Step
	if not d then return false end
	-- the attacked monster may have been destroyed by this battle, in which case
	-- its DEF is read from its last moment on the field
	local def=d:IsLocation(LOCATION_MZONE) and d:GetDefence() or d:GetPreviousDefenseOnField()
	local m=a:GetAttack()-def
	e:SetLabel(m)
	return c==a and def>=0 and m>0 and bit.band(d:GetBattlePosition(),POS_DEFENCE)~=0
end
function c95526884.rectg(e,tp,eg,ep,ev,re,r,rp,chk)
	if chk==0 then return true end
	Duel.SetTargetPlayer(tp)
	Duel.SetTargetParam(e:GetLabel())
	Duel.SetOperationInfo(0,CATEGORY_RECOVER,nil,0,tp,e:GetLabel())
end
function c95526884.recop(e,tp,eg,ep,ev,re,r,rp)
	local p,d=Duel.GetChainInfo(0,CHAININFO_TARGET_PLAYER,CHAININFO_TARGET_PARAM)
	Duel.Recover(p,d,REASON_EFFECT)
end
